/* ==========================================================================
 * state_machine.c —— BMS 状态机【驱动层】
 *
 * 分工：state_machine_core.{h,c} 是平台无关的纯决策内核（只含 <stdint.h>/<stdbool.h>）；
 * 本文件负责把它接到硬件上：
 *   取输入快照 → 调 ts_fsm_step() → 发布结果到 ts_state → 锁外执行动作
 *   （写 GPIO、驱动 AIR、发充电请求、触发预充超时错误源）。
 *
 * 锁纪律（本文件最重要的一条）：
 *   决策态 TsFsmCtx 会被遥测线程的串口 setter（ts_sm_serial_*）并发写，故 ts_sm_update
 *   首次给"快照 + step + 发布"这一段加锁（bms_state_lock 是递归锁）。但**动作一律在锁外**：
 *     - bms_out() 内部会先解锁再打印（bms_io.c），外层若持锁会卡住 UART 日志；
 *     - AIR 的 ts_sm_AIR_close_wait 最多阻塞 ~30ms，持锁会卡住 CAN RX 与遥测线程。
 *   顺带修掉原先无锁读 shunt_data 的隐患（CAN RX 线程在写它）。
 *   裸机版两把锁都是空实现，上面这些约束自动成立。
 * ========================================================================== */

#include "state_machine.h"
#include "bms_io.h"
#include "main.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "can_custom.h"
#include "can-halal.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* 全局状态机的"唯一权威"副本：跨模块（遥测/CAN/bms_io）读它，语义与重构前一致。 */
TSStateHandle ts_state;
uint16_t ts_error_kind = TS_ERROR_NONE;

/* 全部决策态（原文件级 static）集中于此，交由纯内核读写。
 * 遥测线程的 setter 也写其中 control_mode/manual_air_bits/serial_control_timestamp，
 * 故一律持锁访问。air_* 三个字段只被 ctrl 线程在锁外写（AIR 执行在锁外），setter 不碰。 */
static TsFsmCtx fsm_ctx;

static uint16_t ts_sm_error_kind_mask_for_source(uint32_t flag);

void ts_sm_init() {
    ts_state.current_state = TS_INACTIVE;
    ts_state.target_state = TS_INACTIVE;
    ts_state.error = 0;
    ts_error_kind = TS_ERROR_NONE;
    ts_fsm_init(&fsm_ctx);
}

/* 一拍主循环：快照输入 → 纯决策 → 发布 → 锁外执行动作。
 *
 * 原逻辑全程无锁；本次首次加锁：fsm_ctx 会被遥测线程的 setter 写，须一致快照，
 * 顺带修掉原先无锁读 shunt_data（CAN RX 线程在写）的隐患。
 * 锁只包住"快照+step+发布"，引脚/AIR/串口等阻塞动作一律在锁外——bms_out 内部会
 * 解锁再打印，AIR 的延时最多 ~30ms，持锁会卡住 CAN RX 与遥测。 */
void ts_sm_update() {
    uint32_t now = HAL_GetTick();
    TsFsmInputs in;
    TsFsmOut out;
    uint8_t manual_air;
    uint8_t manual_air_bits;

    bms_state_lock();

    /* 串口超时复位须在判 charge_cmd_check 之前，且与原逻辑同序。
     * 这一步会按"本拍"决定 control_mode，故必须早于下面对 control_mode 的任何读。 */
    ts_fsm_apply_serial_timeout(&fsm_ctx, now);

    /* ---- 锁内：一次性取好本拍输入快照，保证同一拍看到的是自洽数据 ---- */
    in.now = now;
    in.current_state = ts_state.current_state;
    in.target_state = ts_state.target_state;
    in.error = (ts_state.error != 0U) ? 1U : 0U;
    in.power_up = (bms_in(POWER_UP_GPIO_Port, POWER_UP_Pin) == GPIO_PIN_SET) ? 1U : 0U;
    in.sdc_closed = sdc_closed ? 1U : 0U;

    /* charge_cmd_check 仅当 GPIO 模式且 POWER_UP 时才跑（对齐原 handle_bms_in），
     * 且必须在读 charge_mode 之前，才能让"本拍刚过期"取值生效 */
    if(fsm_ctx.control_mode == TS_CONTROL_GPIO && in.power_up) {
        charge_cmd_check();
    }
    in.charge_mode = charge_mode;
    in.voltage_bat = shunt_data.voltage_bat;
    in.voltage_veh = shunt_data.voltage_veh;
    in.current = shunt_data.current;

    /* ---- 纯决策 + 发布。这段很快，是唯一持锁的核心。 ---- */
    ts_fsm_step(&fsm_ctx, &in, &out);
    drive_mode_timeout_check();

    ts_state.current_state = out.next_state;
    ts_state.target_state = out.next_target;
    /* 把动作所需的输出先拷到局部，解锁后再用（避免锁外再读内核输出） */
    manual_air = out.manual_air;
    manual_air_bits = out.manual_air_bits;

    bms_state_unlock();

    /* ================= 以下全部锁外 ================= */

    /* 预充超时：内核只置输出位，真正的错误源置位放这里（该调用会取锁，故不能在内核里）。 */
    if(out.assert_precharge_timeout) {
        ts_sm_set_error_source(TS_ERROR_SOURCE_STATE_MACHINE, 1,
                               TS_ERROR_PRECHARGE_TIMEOUT);
    }

    if(manual_air) {
        /* 手动 AIR：直接由 bits 驱动三个继电器，不写 NERROR/ERROR/HV */
        ts_sm_ctrl_AIR_pwr(AIR_POS, (manual_air_bits & AIR_BIT_POS) != 0);
        ts_sm_ctrl_AIR_pwr(AIR_NEG, (manual_air_bits & AIR_BIT_NEG) != 0);
        ts_sm_ctrl_AIR_pwr(AIR_PRE, (manual_air_bits & AIR_BIT_PRE) != 0);
        return;
    }

    /* 部分写：按 out.pins.write 逐位写，复现原机"本拍只改了哪几个引脚"。
     * 这是等价性的关键——例如预充超时进 ERROR 那拍只写 NERROR，BMS_ERROR 不动；
     * CHARGE 且 current<-300 转 ERROR 那拍 HV 停在 SET。整体 apply 会破坏这些电平。 */
    if(out.pins.write & TS_PIN_NERROR) {
        bms_out(BMS_NERROR_GPIO_Port, BMS_NERROR_Pin,
                out.pins.nerror ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    if(out.pins.write & TS_PIN_BMS_ERROR) {
        bms_out(BMS_ERROR_GPIO_Port, BMS_ERROR_Pin,
                out.pins.error ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    if(out.pins.write & TS_PIN_HV_ACTIVE) {
        bms_out(HV_ACTIVE_GPIO_Port, HV_ACTIVE_Pin,
                out.pins.hv_active ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }

    /* AIR 由目标态统一驱动（内部含闭合沿消抖）。放在引脚写之后。 */
    ts_sm_set_AIR_positions(out.next_state);

    if(out.send_charge_req) {
        can_send_charge_req();
    }
}

/* 按状态设置三个继电器的"期望电平"。注意这里传的是目标态 out.next_state；
 * 每拍都用相同目标电平调用同两个 helper，自动复现原行为（含消抖）。 */
void ts_sm_set_AIR_positions(TSState state) {
    switch(state) {
        case TS_INACTIVE:
        case TS_DISCHARGE:
        case TS_ERROR:
            ts_sm_ctrl_AIR_pwr(AIR_POS, 0);
            ts_sm_ctrl_AIR_pwr(AIR_NEG, 0);
            ts_sm_ctrl_AIR_pwr(AIR_PRE, 0);
            break;
        case TS_PRECHARGE:
            ts_sm_ctrl_AIR_pwr(AIR_POS, 0);
            ts_sm_ctrl_AIR_pwr(AIR_NEG, 1);
            ts_sm_ctrl_AIR_pwr(AIR_PRE, 1);
            break;
        case TS_ACTIVE:
        case TS_CHARGE:
            ts_sm_ctrl_AIR_pwr(AIR_POS, 1);
            ts_sm_ctrl_AIR_pwr(AIR_NEG, 1);
            ts_sm_ctrl_AIR_pwr(AIR_PRE, 0);
            break;
        case TS_PRE_HOLD:
            ts_sm_ctrl_AIR_pwr(AIR_POS, 1);
            ts_sm_ctrl_AIR_pwr(AIR_NEG, 1);
            ts_sm_ctrl_AIR_pwr(AIR_PRE, 1);
    }
}

/* 单个继电器控制：先经 ts_sm_AIR_close_wait 施加闭合沿消抖，再写 GPIO。
 * 三个继电器各自持有一个"当前已命令电平"（fsm_ctx.air_*_state），用于检测跳变。 */
void ts_sm_ctrl_AIR_pwr(AIR relay, int closed_cmd) {
    GPIO_PinState state = closed_cmd ? GPIO_PIN_SET : GPIO_PIN_RESET;
    switch(relay) {
        case AIR_POS:
            ts_sm_AIR_close_wait(&fsm_ctx.air_pos_state, closed_cmd);
            bms_out(POS_AIR_CTRL_GPIO_Port, POS_AIR_CTRL_Pin, state);
            break;
        case AIR_NEG:
            ts_sm_AIR_close_wait(&fsm_ctx.air_neg_state, closed_cmd);
            bms_out(NEG_AIR_CTRL_GPIO_Port, NEG_AIR_CTRL_Pin, state);
            break;
        case AIR_PRE:
            ts_sm_AIR_close_wait(&fsm_ctx.air_pre_state, closed_cmd);
            bms_out(PRECHARGE_CTRL_GPIO_Port, PRECHARGE_CTRL_Pin, state);
            break;
    }
}

/* 闭合沿消抖：电平发生跳变且是"闭合"(AIR_cmd!=0)时，保证与上一次闭合动作至少相隔
 * AIR_CLOSE_WAIT；不足则阻塞补足。断开沿不等待。时间戳与状态都存 fsm_ctx（原为 static）。
 * 阻塞发生在本函数内，调用方（ts_sm_update）已确保在锁外。 */
void ts_sm_AIR_close_wait(int *AIR_state, int AIR_cmd) {
    if(*AIR_state != AIR_cmd) {
        *AIR_state = AIR_cmd;
        if(AIR_cmd) {
            uint32_t dt = HAL_GetTick() - fsm_ctx.air_last_close_timestamp;
            if(dt < AIR_CLOSE_WAIT) {
                BMS_DELAY_MS(AIR_CLOSE_WAIT - dt);
            }
            fsm_ctx.air_last_close_timestamp = HAL_GetTick();
        }
    }
}

/* ccs_data 原先只是把充电机数据拷进一个从不被读的缓冲，直接留空 */
void charge_handle_can_message(const uint8_t *data) {
    (void)data;
}

/* 整车 CAN：刷新 drive_mode 心跳时间戳，并从报文 bit2 解析 drive_mode。
 * （时间戳存 fsm_ctx.last_vcu_can_msg；CAN RX 线程写它，原实现也无锁，保持一致。） */
void sm_handle_can_message(const uint8_t *data) {
    fsm_ctx.last_vcu_can_msg = HAL_GetTick();
    drive_mode = (data[1] & 0x04U) ? 1 : 0;
}

/* 行车模式心跳超时：超时则把 drive_mode 复位为 0。drive_mode 归 can_custom.c，
 * 本函数只是它的看门狗，无迁移读 drive_mode，故不并入内核。 */
void drive_mode_timeout_check() {
    if(drive_mode == 1 &&
       HAL_GetTick() - fsm_ctx.last_vcu_can_msg > DRIVE_MODE_TIMEOUT) {
        drive_mode = 0;
    }
}

/* 按"来源"置位/清零错误。多来源可同时报错：每个来源只管它名下那组细分位（kind 掩码），
 * 置位时只动自己的位，不动别人的；清零时同样只清自己。error 是各来源的聚合位。
 * 末尾维护 bit0 哨兵：error==0 → ts_error_kind=TS_ERROR_NONE；否则清掉 NONE 位。 */
void ts_sm_set_error_source(uint32_t flag, int state, TSErrorKind kind) {
    uint16_t source_kind_mask = ts_sm_error_kind_mask_for_source(flag);
    uint16_t active_kind = (uint16_t)kind & source_kind_mask;

    bms_state_lock();
    if(source_kind_mask == 0U) {
        if(state) {
            ts_state.error |= flag;
        }
        else {
            ts_state.error &= ~flag;
        }
    }
    else {
        ts_error_kind &= (uint16_t)~source_kind_mask;
        if(state && active_kind != 0U) {
            ts_state.error |= flag;
            ts_error_kind |= active_kind;
        }
        else {
            ts_state.error &= ~flag;
        }
    }

    if(ts_state.error == 0U) {
        ts_error_kind = TS_ERROR_NONE;
    }
    else {
        ts_error_kind &= (uint16_t)~TS_ERROR_NONE;
    }
    bms_state_unlock();
}

/* 串口指定目标态（0x10 命令）。目标态只允许 INACTIVE/ACTIVE/CHARGE；
 * 一旦接受即切到"串口目标"模式并打时间戳（用于失联检测）。 */
int ts_sm_serial_set_target(TSState state) {
    if(state != TS_INACTIVE && state != TS_ACTIVE && state != TS_CHARGE) {
        return 0;
    }

    bms_state_lock();
    fsm_ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
    fsm_ctx.serial_control_timestamp = HAL_GetTick();
    fsm_ctx.manual_air_bits = 0;
    ts_state.target_state = state;
    bms_state_unlock();
    return 1;
}

/* 串口手动 AIR（0x11 命令，调试用）。
 *   enable!=0：切到手动模式，把 air_bits 直接交给内核驱动三继电器（清 target）。
 *   enable==0：退回串口目标模式，清 bits，并把状态强制回 INACTIVE。
 * 两种分支都刷新串口心跳时间戳。 */
int ts_sm_serial_set_manual_air(int enable, uint8_t air_bits) {
    bms_state_lock();
    fsm_ctx.serial_control_timestamp = HAL_GetTick();
    if(!enable) {
        fsm_ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
        fsm_ctx.manual_air_bits = 0;
        ts_state.target_state = TS_INACTIVE;
        ts_state.current_state = TS_INACTIVE;
        bms_state_unlock();
        return 1;
    }

    /* Manual AIR is an independent relay-output test controlled by the host. */
    air_bits &= AIR_BIT_POS | AIR_BIT_NEG | AIR_BIT_PRE;
    fsm_ctx.control_mode = TS_CONTROL_SERIAL_MANUAL_AIR;
    ts_state.target_state = TS_INACTIVE;
    fsm_ctx.manual_air_bits = air_bits;
    bms_state_unlock();
    return 1;
}

/* 串口清错（一次性）。故障监视器仍在跑，若故障未消会立刻重新置位本来源。 */
int ts_sm_serial_clear_errors(void) {
    /* This is intentionally a one-shot clear.  Fault monitors keep running
     * and can immediately assert their source again if the fault persists. */
    bms_state_lock();
    slaves_clear_errors();
    shunt_clear_errors();
    ts_state.error = 0U;
    ts_error_kind = TS_ERROR_NONE;
    bms_state_unlock();
    return 1;
}

/* 串口心跳：仅在非 GPIO 模式（即处于串口控制中）刷新时间戳，供失联超时用。 */
void ts_sm_serial_heartbeat(void) {
    bms_state_lock();
    if(fsm_ctx.control_mode != TS_CONTROL_GPIO) {
        fsm_ctx.serial_control_timestamp = HAL_GetTick();
    }
    bms_state_unlock();
}

TSControlMode ts_sm_get_control_mode(void) {
    return fsm_ctx.control_mode;
}

/* 返回三个继电器**实际引脚电平**的打包位（读回 GPIO，而非内部期望值）——供遥测上报。 */
uint8_t ts_sm_get_air_bits(void) {
    uint8_t bits = 0;
    if(bms_in(POS_AIR_CTRL_GPIO_Port, POS_AIR_CTRL_Pin) == GPIO_PIN_SET) {
        bits |= AIR_BIT_POS;
    }
    if(bms_in(NEG_AIR_CTRL_GPIO_Port, NEG_AIR_CTRL_Pin) == GPIO_PIN_SET) {
        bits |= AIR_BIT_NEG;
    }
    if(bms_in(PRECHARGE_CTRL_GPIO_Port, PRECHARGE_CTRL_Pin) == GPIO_PIN_SET) {
        bits |= AIR_BIT_PRE;
    }
    return bits;
}

/* 来源 → 细分错误位掩码。某来源只能"认领"它名下的位，多来源互不干扰。 */
static uint16_t ts_sm_error_kind_mask_for_source(uint32_t flag) {
    switch(flag) {
    case TS_ERROR_SOURCE_SLAVE:
        return (uint16_t)(TS_ERROR_SLAVE_OVERVOLT |
                          TS_ERROR_SLAVE_UNDERVOLT |
                          TS_ERROR_SLAVE_OVERTEMP |
                          TS_ERROR_INTERNAL_COMM_ERROR);
    case TS_ERROR_SOURCE_SHUNT:
        return (uint16_t)(TS_ERROR_SHUNT_TIMEOUT |
                          TS_ERROR_SHUNT_OVERCURRENT |
                          TS_ERROR_SHUNT_OVERTEMP |
                          TS_ERROR_VOLTAGE_MISMATCH);
    case TS_ERROR_SOURCE_STATE_MACHINE:
        return TS_ERROR_PRECHARGE_TIMEOUT;
    default:
        return 0U;
    }
}

/* 通用错误去抖：条件持续 delay_ms 才 confirm。special case：行车模式下电流很小(<1A)
 * 时直接判无错（避免行车时误报）。latch 为真则确认后不再自动复位，须显式 clear。 */
bool error_monitor_update(ErrorMonitor *monitor, bool exist) {
    bool result;
    uint32_t now = HAL_GetTick();

    bms_state_lock();

    if(drive_mode == 1 && abs(shunt_data.current) < 1000 && monitor->confirm == false) {
        monitor->confirm = false;
        bms_state_unlock();
        return false;
    }

    if(exist) {
        if(monitor->confirm) {
            bms_state_unlock();
            return true;
        }

        if(!monitor->pending) {
            monitor->pending = true;
            monitor->pending_start = now;
        }

        if(now - monitor->pending_start >= monitor->delay_ms) {
            monitor->confirm = true;
            monitor->pending = false;
        }
    }

    else {
        monitor->pending = false;
        if(!monitor->latch) {
            monitor->confirm = false;
        }
    }

    result = monitor->confirm;
    bms_state_unlock();
    return result;
}

/* 显式清除某监视器（含锁存位），用于串口清错。 */
void error_monitor_clear(ErrorMonitor *monitor) {
    bms_state_lock();
    monitor->pending = false;
    monitor->pending_start = 0;
    monitor->confirm = false;
    bms_state_unlock();
}
