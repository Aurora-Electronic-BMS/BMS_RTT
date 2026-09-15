/* 状态机纯内核宿主编译单测。
 *
 * 只 include state_machine_core.h——core 一旦泄漏平台头，这里立刻编译失败。
 * 显式喂 now，逐例断言 next_state/next_target/out.pins/ctx 时间戳。
 *
 * 关键点：GPIO 模式下 handle_bms_in 总会用输入（power_up/charge_mode）重算 target，
 * 所以想到达 PRECHARGE/PRE_HOLD 的"达标/超时"分支，必须让 power_up=1（→target=ACTIVE）
 * 或 charge_mode=1（→target=CHARGE）。纯靠喂 target_state 是无效的。
 *
 * 用例与内核分支的覆盖对应：
 *   t1  INACTIVE→PRECHARGE / SDC 未闭合停在 INACTIVE
 *   t2  PRECHARGE 达标判定（未达标保持 / 达标→PRE_HOLD）
 *   t3  PRECHARGE 超时→ERROR（只写 NERROR 的部分写陷阱）
 *   t4  PRE_HOLD→ACTIVE / 不达标→ERROR
 *   t5  PRE_HOLD→CHARGE（门槛 0.95·vbat）/ 不达标→ERROR
 *   t6  ACTIVE→DISCHARGE（SDC/串口 target 两条路）+ 保持 ACTIVE
 *   t7  DISCHARGE 边界 4999/5000
 *   t8  CHARGE current<-300→ERROR（HV 停在 SET、不置错误位）+ 正常发请求
 *   t9  ERROR(error=1) 保持
 *   t10 ERROR 恢复（哨兵 + 严格 '>' + <5000 走 DISCHARGE(重锚计时) / >5000 走 INACTIVE）
 *   t11 串口超时复位（>1000 复位 / ==1000 不复位）
 *   t12 manual AIR（POS|NEG / NEG|PRE / 0 / error=1 仍胜出）
 *   t13 handle_bms_in（power_up 目标 + 落沿保持 500ms）
 *   t14 错误覆盖（error=1 压过一切非 manual 分支）
 *   t15 充电请求提前（target==CHARGE 即发，不等预充；SDC/故障/manual 三道护栏）
 */
#include "state_machine_core.h"

#include <stdio.h>
#include <string.h>

static TsFsmCtx ctx;
static TsFsmOut out;
static int checks = 0;
static int fails = 0;

static void ck(int cond, const char *what) {
    checks++;
    if(!cond) {
        fails++;
        printf("    FAIL: %s\n", what);
    }
}

/* 单拍封装：驱动层真实的调用顺序是先 apply_serial_timeout 再 step，这里照做。 */
static TsFsmOut *step(const TsFsmInputs *in) {
    ts_fsm_apply_serial_timeout(&ctx, in->now);
    ts_fsm_step(&ctx, in, &out);
    return &out;
}

/* 造一个"中立"输入：SDC 闭合、电池=车辆侧=425500mV、电流 0。
 * 各用例按需覆盖 power_up、charge_mode、voltage_* 电压、current、error 等字段。 */
static TsFsmInputs base(uint32_t now, TSState cur, TSState tgt) {
    TsFsmInputs in;
    memset(&in, 0, sizeof(in));
    in.now = now;
    in.current_state = cur;
    in.target_state = tgt;
    in.sdc_closed = 1;
    in.voltage_bat = 425500;
    in.voltage_veh = 425500;
    in.current = 0;
    return in;
}

static void t1_inactive_to_precharge(void) {
    printf("1) INACTIVE -> PRECHARGE\n");
    TsFsmInputs in = base(1000, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_PRECHARGE, "next=PRECHARGE");
    ck(o->next_target == TS_ACTIVE, "target=ACTIVE");
    ck(ctx.precharge_start_timestamp == 1000, "precharge_start==now");
    ck(ctx.precharge_95_finished_timestamp == 0, "precharge_95==0");
    ck((o->pins.write & TS_PIN_NERROR) && o->pins.nerror == 1, "NERROR=SET written");

    /* SDC 未闭合：停在 INACTIVE */
    ts_fsm_init(&ctx);
    in = base(1000, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.sdc_closed = 0;
    o = step(&in);
    ck(o->next_state == TS_INACTIVE, "sdc open -> stay INACTIVE");
}

static void t2_precharge_hold(void) {
    printf("2) PRECHARGE 达标判定\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(1000, TS_PRECHARGE, TS_INACTIVE);
    in.power_up = 1;              /* -> target INDIRECTLY ACTIVE */
    in.voltage_bat = 425500;      /* 0.95*425500 = 404225 */
    in.voltage_veh = 400000;      /* 不达标 */
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_PRECHARGE, "veh<0.95*bat -> hold");

    in = base(1100, TS_PRECHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.voltage_bat = 425500;
    in.voltage_veh = 425500;      /* 达标 */
    o = step(&in);
    ck(o->next_state == TS_PRE_HOLD, "veh>0.95*bat -> PRE_HOLD");
    ck(ctx.precharge_95_finished_timestamp == 1100, "precharge_95==now");
}

static void t3_precharge_timeout(void) {
    printf("3) PRECHARGE 超时 -> ERROR（只写 NERROR）\n");
    ts_fsm_init(&ctx);
    /* precharge_start 未设（0），now=5000 → now-start==5000，命中 >= */
    TsFsmInputs in = base(5000, TS_PRECHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.voltage_veh = 100;         /* < MIN_VEHICLE_SIDE_VOLTAGE，且达不到 0.95*bat */
    in.voltage_bat = 425500;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ERROR, "now-start==5000 -> ERROR");
    ck(o->assert_precharge_timeout == 1, "assert set");
    ck(o->pins.write == TS_PIN_NERROR, "only NERROR written");
    ck(o->pins.nerror == 0, "NERROR=RESET");
    ck((o->pins.write & TS_PIN_BMS_ERROR) == 0, "BMS_ERROR NOT written");
}

static void t4_pre_hold_active(void) {
    printf("4) PRE_HOLD -> ACTIVE / 失败 -> ERROR\n");
    ts_fsm_init(&ctx);
    /* precharge_95_finished 未设（0），now=200 → ==PRE_HOLD_TIME，命中 >= */
    TsFsmInputs in = base(200, TS_PRE_HOLD, TS_INACTIVE);
    in.power_up = 1;
    in.voltage_veh = 425500;
    in.voltage_bat = 425500;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ACTIVE, ">=200ms & 达标 -> ACTIVE");

    ts_fsm_init(&ctx);
    in = base(200, TS_PRE_HOLD, TS_INACTIVE);
    in.power_up = 1;
    in.voltage_veh = 300000;
    in.voltage_bat = 425500;
    o = step(&in);
    ck(o->next_state == TS_ERROR, "不达标 -> ERROR");
    ck(o->assert_precharge_timeout == 1, "assert set");
}

static void t5_pre_hold_charge(void) {
    printf("5) PRE_HOLD -> CHARGE（门槛 0.95·vbat）/ 失败 -> ERROR\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(200, TS_PRE_HOLD, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;           /* -> target CHARGE */
    in.voltage_bat = 425500;      /* 门槛 0.95*425500 ≈ 404225 */
    in.voltage_veh = 425500;      /* 达标 */
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_CHARGE, "veh>0.95*bat -> CHARGE");

    ts_fsm_init(&ctx);
    in = base(200, TS_PRE_HOLD, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.voltage_bat = 425500;
    in.voltage_veh = 400000;      /* < 0.95*bat，不达标 */
    o = step(&in);
    ck(o->next_state == TS_ERROR, "veh<0.95*bat -> ERROR");
    ck(o->assert_precharge_timeout == 1, "assert set");
}

static void t6_active_to_discharge(void) {
    printf("6) ACTIVE -> DISCHARGE\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(500, TS_ACTIVE, TS_ACTIVE);
    in.sdc_closed = 0;            /* SDC 拉开 */
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_DISCHARGE, "sdc open -> DISCHARGE");
    ck(ctx.discharge_begin_timestamp == 500, "discharge_begin==now");
    ck((o->pins.write & TS_PIN_HV_ACTIVE) && o->pins.hv_active == 0, "HV=RESET written");

    /* 串口模式：target 可被直接置 INACTIVE（不受 handle_bms_in 重算影响） */
    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
    ctx.serial_control_timestamp = 500;
    in = base(500, TS_ACTIVE, TS_INACTIVE);
    o = step(&in);
    ck(o->next_state == TS_DISCHARGE, "serial target=INACTIVE -> DISCHARGE");

    /* SDC 闭合且无退出条件：保持 ACTIVE */
    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
    ctx.serial_control_timestamp = 500;
    in = base(500, TS_ACTIVE, TS_ACTIVE);
    o = step(&in);
    ck(o->next_state == TS_ACTIVE, "hold ACTIVE");
    ck((o->pins.write & TS_PIN_HV_ACTIVE) && o->pins.hv_active == 1, "HV=SET written");
}

static void t7_discharge_boundary(void) {
    printf("7) DISCHARGE 边界 4999/5000\n");
    ts_fsm_init(&ctx);
    /* discharge_begin 未设（0）：4999<5000 保持，5000 命中 >= 回 INACTIVE */
    TsFsmInputs in = base(4999, TS_DISCHARGE, TS_INACTIVE);
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_DISCHARGE, "4999 -> DISCHARGE");
    in = base(5000, TS_DISCHARGE, TS_INACTIVE);
    o = step(&in);
    ck(o->next_state == TS_INACTIVE, "5000 -> INACTIVE");
}

static void t8_charge_current_negative(void) {
    printf("8) CHARGE current<-300 -> ERROR（不重置 HV）\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(0, TS_CHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;           /* -> target CHARGE，避免被 handle_bms_in 拉回 */
    in.current = -301;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ERROR, "current<-300 -> ERROR");
    ck(o->assert_precharge_timeout == 0, "no precharge assert");
    /* 首行已把 HV 写 SET，current<-300 分支直接返回，故本拍 HV 停在 SET（部分写陷阱） */
    ck((o->pins.write & TS_PIN_HV_ACTIVE) && o->pins.hv_active == 1, "HV stays SET (not reset)");
    ck((o->pins.write & TS_PIN_BMS_ERROR) == 0, "no BMS_ERROR write");
    ck(o->pins.error == 0, "error pin level not asserted");
    ck(o->send_charge_req == 0, "no charge req");

    /* 边界：==-300 不判故障（条件为严格 <），留在 CHARGE 并发请求 */
    ts_fsm_init(&ctx);
    in = base(0, TS_CHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.current = -300;
    o = step(&in);
    ck(o->next_state == TS_CHARGE, "-300 -> stay CHARGE");
    ck(o->send_charge_req == 1, "-300 still sends req");

    /* 正常 CHARGE：发充电请求 */
    ts_fsm_init(&ctx);
    in = base(0, TS_CHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.current = 0;
    o = step(&in);
    ck(o->next_state == TS_CHARGE, "stay CHARGE");
    ck(o->send_charge_req == 1, "send_charge_req");
}

static void t9_error_hold(void) {
    printf("9) ERROR(error=1) 保持\n");
    ts_fsm_init(&ctx);
    ctx.no_error_start = 12345;   /* 预置非零，验证本轮被清 */
    TsFsmInputs in = base(0, TS_ERROR, TS_INACTIVE);
    in.error = 1;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ERROR, "stay ERROR");
    ck(o->pins.write == (TS_PIN_NERROR | TS_PIN_BMS_ERROR | TS_PIN_HV_ACTIVE),
       "writes all three pins");
    ck(o->pins.nerror == 0 && o->pins.error == 1 && o->pins.hv_active == 0,
       "NERROR=0 BMS_ERROR=1 HV=0");
    ck(ctx.no_error_start == 0, "no_error_start cleared");
}

static void t10_error_recovery(void) {
    printf("10) ERROR 恢复（哨兵 + 严格 >）\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(1000, TS_ERROR, TS_INACTIVE);
    in.error = 0;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ERROR, "首拍仍 ERROR");
    ck(ctx.no_error_start == 1000, "no_error_start 记录");
    ck(o->pins.nerror == 0 && (o->pins.write & TS_PIN_BMS_ERROR) && o->pins.error == 1,
       "仍写 NERROR=0 / BMS_ERROR=SET");

    in = base(2000, TS_ERROR, TS_INACTIVE);      /* 2000-1000 == 1000，不 > */
    o = step(&in);
    ck(o->next_state == TS_ERROR, "==1000 仍 ERROR");

    in = base(2001, TS_ERROR, TS_INACTIVE);      /* 1001 > 1000 */
    o = step(&in);
    ck(o->next_state == TS_DISCHARGE, "1001 -> DISCHARGE（<5000）");
    ck((o->pins.write & TS_PIN_NERROR) && o->pins.nerror == 1, "恢复 NERROR=SET");
    ck((o->pins.write & TS_PIN_BMS_ERROR) && o->pins.error == 0, "恢复 BMS_ERROR=RESET");
    ck(ctx.discharge_begin_timestamp == 2001, "重锚 discharge_begin==now");
    ck(ctx.no_error_start == 0, "哨兵清零");

    /* 超过 DISCHARGE_DURATION 的恢复走 INACTIVE（now=0 会与哨兵 0 相撞，故从 1 起） */
    ts_fsm_init(&ctx);
    in = base(1, TS_ERROR, TS_INACTIVE);
    in.error = 0;
    step(&in);                                    /* no_error_start=1 */
    in = base(6001, TS_ERROR, TS_INACTIVE);
    o = step(&in);
    ck(o->next_state == TS_INACTIVE, ">5000 恢复 -> INACTIVE");
}

static void t11_serial_timeout(void) {
    printf("11) 串口超时复位\n");
    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
    ctx.serial_control_timestamp = 0;
    ctx.manual_air_bits = 0x7;
    TsFsmInputs in = base(1001, TS_INACTIVE, TS_INACTIVE);
    step(&in);
    ck(ctx.control_mode == TS_CONTROL_GPIO, "1001 > 1000 -> GPIO");
    ck(ctx.manual_air_bits == 0, "bits 清零");

    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_TARGET;
    ctx.serial_control_timestamp = 0;
    in = base(1000, TS_INACTIVE, TS_INACTIVE);
    step(&in);
    ck(ctx.control_mode == TS_CONTROL_SERIAL_TARGET, "==1000 不复位");
}

static void t12_manual_air(void) {
    printf("12) manual AIR\n");
    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_MANUAL_AIR;
    ctx.serial_control_timestamp = 100;           /* 防超时复位 */
    TsFsmInputs in = base(100, TS_INACTIVE, TS_INACTIVE);

    ctx.manual_air_bits = AIR_BIT_POS | AIR_BIT_NEG;   /* 0b011 */
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ACTIVE, "POS|NEG -> ACTIVE");
    ck(o->manual_air == 1 && o->manual_air_bits == (AIR_BIT_POS | AIR_BIT_NEG),
       "manual flag+bits");
    ck(o->pins.write == 0, "不写引脚");

    ctx.manual_air_bits = AIR_BIT_NEG | AIR_BIT_PRE;   /* 0b110 */
    o = step(&in);
    ck(o->next_state == TS_PRECHARGE, "NEG|PRE -> PRECHARGE");

    ctx.manual_air_bits = 0;
    o = step(&in);
    ck(o->next_state == TS_INACTIVE, "0 -> INACTIVE");

    /* manual 分支在错误覆盖之后，故 error=1 时 manual 仍胜出，且不写错误引脚 */
    ctx.manual_air_bits = AIR_BIT_POS | AIR_BIT_NEG;
    in.error = 1;
    o = step(&in);
    ck(o->next_state == TS_ACTIVE, "error=1 时 manual 仍胜出");
    ck(o->pins.write == 0, "manual 不写错误引脚");
}

static void t13_handle_bms_in(void) {
    printf("13) handle_bms_in\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 0;
    TsFsmOut *o = step(&in);
    ck(o->next_target == TS_ACTIVE, "power_up -> target ACTIVE");
    ck(o->next_state == TS_PRECHARGE, "INACTIVE+sdc -> PRECHARGE");
    in = base(100, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    o = step(&in);
    ck(o->next_target == TS_CHARGE, "power_up+charge -> target CHARGE");

    /* 落沿保持：ACTIVE 且 tgt 保持 500ms 后回 INACTIVE（MAX_STATE_HOLD_TIME，<= 含端点） */
    ts_fsm_init(&ctx);
    in = base(1000, TS_ACTIVE, TS_ACTIVE);
    in.power_up = 0;
    o = step(&in);
    ck(ctx.state_convert_timestamp == 1000, "记戳");
    ck(o->next_target == TS_ACTIVE, "首拍保持");
    in = base(1500, TS_ACTIVE, TS_ACTIVE);
    in.power_up = 0;
    o = step(&in);
    ck(o->next_target == TS_ACTIVE, "==500 保持");
    in = base(1501, TS_ACTIVE, TS_ACTIVE);
    in.power_up = 0;
    o = step(&in);
    ck(o->next_target == TS_INACTIVE, "501 -> INACTIVE");
    ck(ctx.state_convert_timestamp == 0, "哨兵清零");
}

static void t14_error_override(void) {
    printf("14) 错误覆盖\n");
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.error = 1;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_ERROR, "error=1 覆盖 -> ERROR");
}

/* 充电请求提前：不再要求先走完预充（破解"充电机必须先收到请求才输出预充电压"的死锁）。
 * 判定 = target==CHARGE && 未落故障 && SDC 闭合，与当前状态无关。 */
static void t15_charge_req_early(void) {
    printf("15) 充电请求提前发送\n");

    /* 空闲态、刚收到充电机报文（charge_mode=1）→ 当拍就回发请求 */
    ts_fsm_init(&ctx);
    TsFsmInputs in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    TsFsmOut *o = step(&in);
    ck(o->next_state == TS_PRECHARGE, "INACTIVE+sdc -> PRECHARGE");
    ck(o->next_target == TS_CHARGE, "target=CHARGE");
    ck(o->send_charge_req == 1, "预充之前就发请求");

    /* 预充中、电压远不达标 → 仍发（这正是原实现发不出去的那一拍） */
    ts_fsm_init(&ctx);
    in = base(100, TS_PRECHARGE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.voltage_veh = 100;             /* 连 150mV 下限都不到，停在 PRECHARGE */
    o = step(&in);
    ck(o->next_state == TS_PRECHARGE, "stay PRECHARGE");
    ck(o->send_charge_req == 1, "预充中继续发请求");

    /* 护栏 1：SDC 断开 → 不发 */
    ts_fsm_init(&ctx);
    in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.sdc_closed = 0;
    o = step(&in);
    ck(o->send_charge_req == 0, "SDC 断开不发请求");

    /* 护栏 2：有故障 → 不发（error 输入把状态压成 ERROR） */
    ts_fsm_init(&ctx);
    in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    in.charge_mode = 1;
    in.error = 1;
    o = step(&in);
    ck(o->next_state == TS_ERROR, "error -> ERROR");
    ck(o->send_charge_req == 0, "有故障不发请求");

    /* 无充电意图（charge_mode=0 → target=ACTIVE）→ 不发 */
    ts_fsm_init(&ctx);
    in = base(0, TS_INACTIVE, TS_INACTIVE);
    in.power_up = 1;
    o = step(&in);
    ck(o->next_target == TS_ACTIVE, "target=ACTIVE");
    ck(o->send_charge_req == 0, "非充电意图不发请求");

    /* 护栏 3：manual AIR 分支在 ⑤ 已 return，即使 target 是 CHARGE 也不发 */
    ts_fsm_init(&ctx);
    ctx.control_mode = TS_CONTROL_SERIAL_MANUAL_AIR;
    ctx.serial_control_timestamp = 0;
    ctx.manual_air_bits = AIR_BIT_POS | AIR_BIT_NEG;
    in = base(0, TS_INACTIVE, TS_CHARGE);
    o = step(&in);
    ck(o->manual_air == 1, "走 manual 分支");
    ck(o->send_charge_req == 0, "manual 模式不发请求");
}

int main(void) {
    t1_inactive_to_precharge();
    t2_precharge_hold();
    t3_precharge_timeout();
    t4_pre_hold_active();
    t5_pre_hold_charge();
    t6_active_to_discharge();
    t7_discharge_boundary();
    t8_charge_current_negative();
    t9_error_hold();
    t10_error_recovery();
    t11_serial_timeout();
    t12_manual_air();
    t13_handle_bms_in();
    t14_error_override();
    t15_charge_req_early();

    printf("\n=== %d/%d checks passed ===\n", checks - fails, checks);
    return fails ? 1 : 0;
}
