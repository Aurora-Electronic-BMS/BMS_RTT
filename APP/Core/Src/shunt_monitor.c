/* ==========================================================================
 * shunt_monitor.c —— 分流器（电流/电压/温度）数据与保护
 *
 * 数据来源：CAN1 的 0x52x 段帧，由 CAN RX 线程经 shunt_handle_can_message() 写入
 * global shunt_data；控制线程每 150ms 调 shunt_check() 做保护判定。
 * 四个 ErrorMonitor（超时/过流/过温/电压失配）去抖后统一经 ts_sm_set_error_source()
 * 报到状态机（来源 TS_ERROR_SOURCE_SHUNT）。
 *
 * 锁：shunt_data 会被 CAN RX 线程写、控制/遥测线程读，故所有读写都在 state_mtx 内。
 * ========================================================================== */
#include "shunt_monitor.h"
#include "bms_io.h"
#include <string.h>
#include "can_custom.h"
#include "slave_monitor.h"
#include "state_machine.h"
#include "can-halal.h"
#include "main.h"
#include <stdlib.h>

ShuntData shunt_data;
int shunt_error = 0;

/* 四个保护的去抖时长：超时/过流 500ms，过温/失配 1000ms；均锁存（须显式清错）。 */
static ErrorMonitor shunt_timeout_monitor = ERROR_MONITOR_INIT(500, ERROR_LATCH_ENABLE);
static ErrorMonitor shunt_overcurrent_monitor = ERROR_MONITOR_INIT(500, ERROR_LATCH_ENABLE);
static ErrorMonitor shunt_overtemp_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);
static ErrorMonitor shunt_voltage_mismatch_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);

void shunt_init() {
    shunt_data.current = 0;
    shunt_data.voltage_bat = 0;
    shunt_data.voltage_veh = 0;
    shunt_data.voltage_x = 0;
    shunt_data.busbartemp = 0;
    shunt_data.power = 0;
    shunt_data.energy = 0;
    shunt_data.current_counter = 0;
    shunt_data.last_message = 0;
    shunt_data.last_current_message = 0;
}

/* 串口清错：复位四个监视器（含锁存）。 */
void shunt_clear_errors() {
    bms_state_lock();
    error_monitor_clear(&shunt_timeout_monitor);
    error_monitor_clear(&shunt_overcurrent_monitor);
    error_monitor_clear(&shunt_overtemp_monitor);
    error_monitor_clear(&shunt_voltage_mismatch_monitor);
    bms_state_unlock();
}

/* 保护巡检（150ms 一次）：四条判定同时跑，命中即置对应细分错误位；
 * 电压失配另外驱动 HV_MISMATCH 引脚。最后把聚合结果上报状态机。 */
void shunt_check() {
    TSErrorKind kind = (TSErrorKind)0U;

    /* 全程持锁：shunt_data 由 CAN RX 线程写、这里读，需一致快照 */
    bms_state_lock();
    /* 超时：距最近一帧 > SHUNT_TIMEOUT(500ms)。注意注入会刷新 last_message，
     * 故要触发超时必须**停止**注入，而非注入 0。 */
    if(error_monitor_update(&shunt_timeout_monitor, HAL_GetTick() - shunt_data.last_message > SHUNT_TIMEOUT)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SHUNT_TIMEOUT);
    }
    /* 过流：放电方向 < -200A 或充电方向 > 55A（阈值见 shunt_monitor.h）。 */
    if(error_monitor_update(&shunt_overcurrent_monitor, shunt_data.current < -SHUNT_OVERCURRENT_DISCHARGE || shunt_data.current > SHUNT_OVERCURRENT_CHARGE)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SHUNT_OVERCURRENT);
    }
    /* 过温：母线温度 > 100.0℃。 */
    if(error_monitor_update(&shunt_overtemp_monitor, shunt_data.busbartemp > SHUNT_OVERTEMP)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SHUNT_OVERTEMP);
    }
    /* 电压失配：AFE 汇总电压 Bat_Volt 与分流器上报电压_bat 相差 > 10V。
     * 同时直接驱动 HV_MISMATCH 引脚（无监视器，即时反映）。 */
    if(error_monitor_update(&shunt_voltage_mismatch_monitor, abs(Bat_Volt - (int32_t)shunt_data.voltage_bat) > 10000)) {
        kind = (TSErrorKind)(kind | TS_ERROR_VOLTAGE_MISMATCH);
        bms_out(HV_MISMATCH_GPIO_Port, HV_MISMATCH_Pin, GPIO_PIN_SET);
    }
    else {
        bms_out(HV_MISMATCH_GPIO_Port, HV_MISMATCH_Pin, GPIO_PIN_RESET);
    }

    shunt_error = (kind != 0U);
    ts_sm_set_error_source(TS_ERROR_SOURCE_SHUNT, shunt_error, kind);
    bms_state_unlock();
}

/* 电流的 mAs 级累计（安时积分的数据源）：首帧只记时间戳，之后按 dt 积分。
 * shunt_data.current 须已更新。调用者需持 state_mtx。 */
static void shunt_accumulate_current(void) {
    if(shunt_data.last_current_message > 0) {
        uint32_t now = HAL_GetTick();
        float dt = now - shunt_data.last_current_message;
        shunt_data.current_counter += shunt_data.current * dt * 0.001f;
    }
    shunt_data.last_current_message = HAL_GetTick();
}

#if BMS_TEST_HOOKS
/* 故障注入（逻辑验证用）：直接改写 shunt_data 字段，绕过 CAN。返回 0 成功、-1 未知 item。 */
int shunt_inject(uint8_t item, int32_t value) {
    bms_state_lock();
    switch(item) {
    case BMS_FI_SHUNT_CURRENT:
        shunt_data.current = value;
        /* 与 CAN 电流帧同路径推进累计，否则注入只改瞬时电流、安时积分不动，
         * SOC 在台架上无法被驱动。 */
        shunt_accumulate_current();
        break;
    case BMS_FI_SHUNT_VOLTAGE_BAT:
        shunt_data.voltage_bat = value;
        break;
    case BMS_FI_SHUNT_VOLTAGE_VEH:
        shunt_data.voltage_veh = value;
        break;
    case BMS_FI_SHUNT_BUSBAR_TEMP:
        shunt_data.busbartemp = value;
        break;
    default:
        bms_state_unlock();
        return -1;
    }
    /* 注入即视为"分流器在报数"，刷新时间戳；测 SHUNT_TIMEOUT 靠停止注入 */
    shunt_data.last_message = HAL_GetTick();
    bms_state_unlock();
    return 0;
}
#endif

/* CAN RX 线程入口：解析 0x52x 段每种帧并写入 shunt_data 对应字段。
 * 电流帧额外做 mAs 级电流累计（current_counter）。 */
void shunt_handle_can_message(uint16_t id, const uint8_t *data) {
    /* CAN RX 线程写 shunt_data；与控制线程的读互斥 */
    bms_state_lock();
    //HAL_GPIO_TogglePin(STATUS_5_GPIO_Port, STATUS_5_Pin);
    shunt_data.last_message = HAL_GetTick();
    uint64_t message = ftcan_unmarshal_unsigned(&data, 6);

    switch(id) {
    case CAN_ID_SHUNT_CURRENT:
        shunt_data.current = message;
        shunt_accumulate_current();
        break;
    case CAN_ID_SHUNT_VOLTAGE_1:
        shunt_data.voltage_x = message;
        break;
    case CAN_ID_SHUNT_VOLTAGE_2:
        shunt_data.voltage_veh = message;
        break;
    case CAN_ID_SHUNT_VOLTAGE_3:
        shunt_data.voltage_bat = message;
        break;
    case CAN_ID_SHUNT_BUSBAR_TEMP:
        shunt_data.busbartemp = message;
        break;
    case CAN_ID_SHUNT_POWER:
        shunt_data.power = message;
        break;
    case CAN_ID_SHUNT_CURRENT_COUNTER:
        //SHUNT输出位As级别
        break;
    case CAN_ID_SHUNT_ENERGY_COUNTER:
        shunt_data.energy = message;
        break;
    }
    bms_state_unlock();
}
