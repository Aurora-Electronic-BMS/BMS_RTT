#include "state_machine_core.h"

/* ==========================================================================
 * state_machine_core.c —— 状态机纯决策内核的**唯一**实现
 *
 * 设计说明、三条硬约束、以及"原 ts_sm_xxx()"注释约定，见 state_machine_core.h 头部。
 * 本文件只做一件事：把"输入快照 + 决策态"喂进去，算出"下一态 + 本拍该写哪几个引脚"。
 * 除读写 ctx/out 外**没有任何副作用**（不碰引脚、不取锁、不延时、不读时钟）。
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 【原实现对照表】纯内核函数 ←→ 重构前 state_machine.c 里的对应逻辑
 * --------------------------------------------------------------------------
 * 函数名是稳定锚点；重构前的行号仅供追溯，不再逐处标注。逐条迁移条件、每个
 * 引脚电平、每个时间阈值都必须与本表右侧的旧实现**逐拍一致**。
 *
 *   本文件                                旧 state_machine.c
 *   ------------------------------------  ------------------------------------
 *   ts_fsm_init()                         原 static 全零初始化 + control_mode 默认 GPIO
 *   ts_fsm_apply_serial_timeout()         ts_sm_update()      :43-47  串口超时复位前导
 *   ts_fsm_step() 的 error 覆盖           ts_sm_update()      :49-51  "任一错误源→ERROR"
 *   ts_fsm_step() 的 handle_bms_in 块     ts_sm_handle_bms_in()        POWER_UP/CHARGE→target
 *   ts_fsm_step() 的 manual 分支          ts_sm_update()      :57-71  手动 AIR 直驱
 *   ts_fsm_step() 的 switch               ts_sm_update()      :73-95   逐态分发（无 default）
 *   fsm_inactive()                        ts_sm_update_inactive()
 *   fsm_active()                          ts_sm_update_active()
 *   fsm_precharge()                       ts_sm_update_precharge()
 *   fsm_discharge()                       ts_sm_update_discharge()
 *   fsm_error()                           ts_sm_update_error()
 *   fsm_charge()                          ts_sm_update_charge()
 *   fsm_pre_hold()                        ts_sm_update_pre_hold()
 *   fsm_precharge_timeout()               ts_sm_precharge_timeout()     :466-472
 *
 * 唯一有意偏差（见头文件第八节）：旧实现各态内各自再读一次 HAL_GetTick()，纯化后
 * 统一用入口的单个 in->now。最多差一个 50ms 拍，仅在调用期间恰好跨 tick 才有别。
 * -------------------------------------------------------------------------- */

/* 前向声明：fsm_precharge / fsm_pre_hold 在两处超时分支里都要调到它。
 * 旧实现里 ts_sm_precharge_timeout() 也是独立函数，此处保持同名同义。 */
static TSState fsm_precharge_timeout(TsFsmCtx *ctx, TsFsmOut *out);

/* 复位决策态到"刚上电"。等价于旧实现的"文件级 static 全部零初始化" +
 * "control_mode 落到默认值 TS_CONTROL_GPIO"。必须在创建线程/首次 step 之前调用一次。 */
void ts_fsm_init(TsFsmCtx *ctx) {
    *ctx = (TsFsmCtx){0};
    ctx->control_mode = TS_CONTROL_GPIO;
}

/* 串口控制超时复位。仅改 control_mode/manual_air_bits。
 * 原逻辑此处还置 target=INACTIVE，但紧随其后的 handle_bms_in 在 GPIO 模式下总会重算
 * target，故该赋值实际无效；由 ts_fsm_step 重算即可等价复现。
 * 驱动层须在 ts_fsm_step 之前调用它（用于判断本拍是否该跑 charge_cmd_check）。
 * 阈值用严格 '>'，与旧实现一致：恰好 ==SERIAL_CONTROL_TIMEOUT 的那拍**不复位**。 */
void ts_fsm_apply_serial_timeout(TsFsmCtx *ctx, uint32_t now) {
    if(ctx->control_mode != TS_CONTROL_GPIO &&
       now - ctx->serial_control_timestamp > SERIAL_CONTROL_TIMEOUT) {
        ctx->control_mode = TS_CONTROL_GPIO;
        ctx->manual_air_bits = 0U;
    }
}

/* --------------------------------------------------------------------------
 * INACTIVE（空闲）—— 旧 ts_sm_update_inactive()
 * 本态无条件把 NERROR 拉高（"无故障"指示），随后按 target 决定是否进预充。
 * 注意进入预充的两个前提都要满足：target 已指向 ACTIVE/CHARGE **且** SDC 闭合。
 * 记 precharge_start_timestamp 作为预充超时计时起点；precharge_95 清 0。
 * -------------------------------------------------------------------------- */
static TSState fsm_inactive(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    out->pins.write |= TS_PIN_NERROR;
    out->pins.nerror = 1U;
    if(out->next_target == TS_ACTIVE || out->next_target == TS_CHARGE) {
        if(in->sdc_closed) {
            ctx->precharge_start_timestamp = in->now;
            ctx->precharge_95_finished_timestamp = 0U;
            return TS_PRECHARGE;
        }
        return TS_INACTIVE;
    }
    return TS_INACTIVE;
}

/* --------------------------------------------------------------------------
 * ACTIVE（运行）—— 旧 ts_sm_update_active()
 * 首行无条件把 HV_ACTIVE 拉高。退出条件：target 被拉到 INACTIVE，或 SDC 断开。
 * 退出时同拍把 HV_ACTIVE 复位（hv_active=0）并记 discharge_begin_timestamp，
 * 转 DISCHARGE 等母线泄放。
 * -------------------------------------------------------------------------- */
static TSState fsm_active(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    out->pins.write |= TS_PIN_HV_ACTIVE;
    out->pins.hv_active = 1U;
    if(out->next_target == TS_INACTIVE || !in->sdc_closed) {
        ctx->discharge_begin_timestamp = in->now;
        out->pins.hv_active = 0U;
        return TS_DISCHARGE;
    }
    return TS_ACTIVE;
}

/* --------------------------------------------------------------------------
 * PRECHARGE（预充）—— 旧 ts_sm_update_precharge()
 * 先判退出（target=INACTIVE 或 SDC 断开）→ DISCHARGE。
 * 再判达标：车辆侧电压既要有 MIN_VEHICLE_SIDE_VOLTAGE 的下限，又要 > 0.95·vbat。
 *   达标 → 记 precharge_95_finished_timestamp，转 PRE_HOLD。
 *   未达标但已到 PRECHARGE_95_WAIT_TIME（>=，含端点）→ 预充超时。
 *   否则继续停在 PRECHARGE。
 * 末尾 target 既非 ACTIVE/CHARGE 也非 INACTIVE 时落 INACTIVE（旧实现同）。
 * -------------------------------------------------------------------------- */
static TSState fsm_precharge(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    if(out->next_target == TS_INACTIVE || !in->sdc_closed) {
        ctx->discharge_begin_timestamp = in->now;
        return TS_DISCHARGE;
    }

    if(out->next_target == TS_ACTIVE || out->next_target == TS_CHARGE) {
        /* 浮点保持 double 提升，勿收窄成 float：0.95 是 double 字面量，
         * 与 int32 的 voltage_bat 相乘时按 double 运算，收窄会改变边界拍。 */
        if(in->voltage_veh > MIN_VEHICLE_SIDE_VOLTAGE &&
           in->voltage_veh > 0.95 * in->voltage_bat) {
            ctx->precharge_95_finished_timestamp = in->now;
            return TS_PRE_HOLD;
        }
        else if(in->now - ctx->precharge_start_timestamp >= (uint32_t)bms_config.precharge_95_wait_time_ms) {
            return fsm_precharge_timeout(ctx, out);
        }
        return TS_PRECHARGE;
    }

    return TS_INACTIVE;
}

/* --------------------------------------------------------------------------
 * DISCHARGE（放电驻留）—— 旧 ts_sm_update_discharge()
 * 只做一件事：进入本态满 DISCHARGE_DURATION（>=，含端点）就回 INACTIVE，否则停在
 * DISCHARGE。本态不写任何引脚（out 仅用于保持签名一致，故 (void)out）。
 * -------------------------------------------------------------------------- */
static TSState fsm_discharge(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    (void)out;
    if(in->now - ctx->discharge_begin_timestamp >= (uint32_t)bms_config.discharge_duration_ms) {
        return TS_INACTIVE;
    }
    return TS_DISCHARGE;
}

/* --------------------------------------------------------------------------
 * ERROR（故障）—— 旧 ts_sm_update_error()
 * 首行无条件拉低 HV_ACTIVE（对应旧实现 ts_sm_update_error 的首行）。
 *
 * 恢复流程（error==0 时）：
 *   - no_error_start==0（哨兵）：记首拍时间，本拍**仍停在 ERROR**（往下走到底部写 NERROR/BMS_ERROR）。
 *   - 已开始观察且 now-no_error_start > NO_ERROR_TIME（严格 '>'）：进入恢复——
 *       写 NERROR=SET、BMS_ERROR=RESET；
 *       若 now-no_error_start < DISCHARGE_DURATION → 回 DISCHARGE（母线还没泄放完），
 *       否则回 INACTIVE。两条路都把 no_error_start 清 0。
 *
 * 有错（error!=0）时：清 no_error_start，落到函数底部统一写 NERROR=RESET、BMS_ERROR=SET，
 * 停 ERROR。
 *
 * 与旧实现的一处有意偏差：
 *   回 DISCHARGE 那拍补写 discharge_begin_timestamp 重锚放电计时——旧实现没写，
 *   于是 DISCHARGE 会拿一个很旧的计时戳立刻判到点、跳过母线泄放窗口直接回 INACTIVE。
 * -------------------------------------------------------------------------- */
static TSState fsm_error(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    /* 先无条件拉低 HV_ACTIVE（对应原 ts_sm_update_error 首行） */
    out->pins.write |= TS_PIN_HV_ACTIVE;
    out->pins.hv_active = 0U;

    if(in->error == 0U) {
        if(ctx->no_error_start == 0U) {
            ctx->no_error_start = in->now;      /* 哨兵：记首拍，本拍仍停在 ERROR */
        }
        else if(in->now - ctx->no_error_start > (uint32_t)bms_config.no_error_time_ms) {
            out->pins.write |= TS_PIN_NERROR;
            out->pins.nerror = 1U;
            out->pins.write |= TS_PIN_BMS_ERROR;
            out->pins.error = 0U;
            if(in->now - ctx->no_error_start < (uint32_t)bms_config.discharge_duration_ms) {
                ctx->no_error_start = 0U;
                ctx->discharge_begin_timestamp = in->now;
                return TS_DISCHARGE;
            }
            ctx->no_error_start = 0U;
            return TS_INACTIVE;
        }
    }
    else {
        ctx->no_error_start = 0U;
    }

    out->pins.write |= TS_PIN_NERROR;
    out->pins.nerror = 0U;
    out->pins.write |= TS_PIN_BMS_ERROR;
    out->pins.error = 1U;
    return TS_ERROR;
}

/* --------------------------------------------------------------------------
 * CHARGE（充电）—— 旧 ts_sm_update_charge()
 * 首行无条件把 HV_ACTIVE 拉高（**这很关键**：下一处反向电流转 ERROR 时并不复位
 * HV，于是本拍 HV 停在 SET——这是被遥测与 LED1 观测的"部分写"语义，见头文件）。
 * 判退出（target=INACTIVE 或 SDC 断开）→ 记时戳、HV 复位、转 DISCHARGE。
 * 本态**不再**负责发充电请求：发送已上移到 ts_fsm_step 末尾（⑦），只要有充电
 * 意图就发，不再要求已进入本态。
 *
 * 故障门限：反向(放电)电流要超过 CHARGE_MAX_REVERSE_CURRENT 才判 ERROR。对齐新版
 * BMS_SOFTWARE 的 -300——原实现是 current<0 就判，分流器零点漂移/纹波会误报。
 * -------------------------------------------------------------------------- */
static TSState fsm_charge(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    /* 首行无条件拉高 HV_ACTIVE；反向电流转 ERROR 时不动 HV（保持本拍所写的 SET） */
    out->pins.write |= TS_PIN_HV_ACTIVE;
    out->pins.hv_active = 1U;
    if(out->next_target == TS_INACTIVE || !in->sdc_closed) {
        ctx->discharge_begin_timestamp = in->now;
        out->pins.hv_active = 0U;
        return TS_DISCHARGE;
    }
    if(in->current < -CHARGE_MAX_REVERSE_CURRENT) {
        return TS_ERROR;
    }
    return TS_CHARGE;
}

/* --------------------------------------------------------------------------
 * PRE_HOLD（预充保持）—— 旧 ts_sm_update_pre_hold()
 * 先判退出（target=INACTIVE 或 SDC 断开）→ DISCHARGE。
 * 计时窗口：自 precharge_95_finished_timestamp 起满 PRE_HOLD_TIME（>=，含端点）才做终判：
 *   - target==ACTIVE：veh > 0.95·bat → ACTIVE；否则超时。
 *   - target==CHARGE：veh > 0.95·bat → CHARGE；否则超时。
 *     注意与 ACTIVE 同判据（对齐新版 BMS_SOFTWARE）：原实现是 veh > bat，但充电机把
 *     电压顶到≈电池电压就封顶了，永远不可能"超过"，那会必然预充超时。
 *   - 其它 target：记时戳回 DISCHARGE。
 * 窗口未满则停在 PRE_HOLD。
 * -------------------------------------------------------------------------- */
static TSState fsm_pre_hold(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    if(out->next_target == TS_INACTIVE || !in->sdc_closed) {
        ctx->discharge_begin_timestamp = in->now;
        return TS_DISCHARGE;
    }
    if(in->now - ctx->precharge_95_finished_timestamp >= (uint32_t)bms_config.pre_hold_time_ms) {
        if(out->next_target == TS_ACTIVE) {
            if(in->voltage_veh > 0.95 * in->voltage_bat) {
                return TS_ACTIVE;
            }
            return fsm_precharge_timeout(ctx, out);
        }
        if(out->next_target == TS_CHARGE) {
            if(in->voltage_veh > 0.95 * in->voltage_bat) {
                return TS_CHARGE;
            }
            return fsm_precharge_timeout(ctx, out);
        }
        ctx->discharge_begin_timestamp = in->now;
        return TS_DISCHARGE;
    }
    return TS_PRE_HOLD;
}

/* --------------------------------------------------------------------------
 * 预充超时 —— 旧 ts_sm_precharge_timeout() :466-472
 * 纯内核里只置输出位 assert_precharge_timeout，由驱动层锁外调 ts_sm_set_error_source
 * （内核不得有副作用，且该调用会取锁）。清 precharge_95 计时戳。
 * 【最易踩的回归点】本拍**只写 NERROR=RESET**，BMS_ERROR 那拍根本不动（保持 0）。
 * 若驱动层按"目标态 ERROR"整体 apply 引脚，就会提前把 BMS_ERROR 拉高。
 * -------------------------------------------------------------------------- */
static TSState fsm_precharge_timeout(TsFsmCtx *ctx, TsFsmOut *out) {
    ctx->precharge_95_finished_timestamp = 0U;
    out->assert_precharge_timeout = 1U;
    /* 只写 NERROR=RESET，BMS_ERROR 本拍不写（保持 0）——最易踩的回归点 */
    out->pins.write |= TS_PIN_NERROR;
    out->pins.nerror = 0U;
    return TS_ERROR;
}

/* ==========================================================================
 * 推进一拍。除读写 ctx/out 外无副作用——所有输入经 in 传入，所有决策经 out 传出。
 * 前导顺序**不可变**，逐条对应旧 ts_sm_update() 的语句顺序：
 *   ① 清空 out（所有"本拍写"位与输出位归零）
 *   ② 错误覆盖：任一错误源置位即强制 ERROR（后续分支一律用覆盖后的 cur）
 *   ③ handle_bms_in：仅 GPIO 模式（串口目标/手动模式的 target 由 setter 直写）
 *   ④ 发布 next_state/next_target
 *   ⑤ manual AIR 分支（命中即 return）
 *   ⑥ 逐态 switch
 *   ⑦ 充电请求置位（有充电意图且本拍未落故障、SDC 闭合）
 * ========================================================================== */
void ts_fsm_step(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out) {
    TSState cur = in->current_state;
    TSState tgt = in->target_state;
    uint32_t now = in->now;

    /* ① 清零：本拍尚未写任何引脚，也未触发任何动作 */
    out->pins.write = 0U;
    out->pins.nerror = 0U;
    out->pins.error = 0U;
    out->pins.hv_active = 0U;
    out->manual_air = 0U;
    out->manual_air_bits = 0U;
    out->send_charge_req = 0U;
    out->assert_precharge_timeout = 0U;

    /* ② 错误覆盖：任一错误源置位即强制 ERROR（后续分支用覆盖后的 cur） */
    if(in->error) {
        cur = TS_ERROR;
    }

    /* ③ handle_bms_in：仅 GPIO 模式（串口目标/手动模式的 target 由 setter 直写）。
     *   - POWER_UP 有效：target = charge_mode ? CHARGE : ACTIVE（charge_mode 由驱动层
     *     在取快照前跑 charge_cmd_check 刷新的全局变量带入）。
     *   - POWER_UP 落下且当前在 ACTIVE：先记 state_convert_timestamp（哨兵 0 表示"还没
     *     开始计时"），并在 bms_config.max_state_hold_time_ms 内（<=，含端点）保持
     *     target=ACTIVE，否则清哨兵、target=INACTIVE。这是"延时下电"的保持窗口。 */
    if(ctx->control_mode == TS_CONTROL_GPIO) {
        if(in->power_up) {
            tgt = (in->charge_mode == 1U) ? TS_CHARGE : TS_ACTIVE;
        }
        else if(cur == TS_ACTIVE && ctx->state_convert_timestamp == 0U) {
            ctx->state_convert_timestamp = now;
            tgt = TS_ACTIVE;
        }
        else if(cur == TS_ACTIVE &&
                now - ctx->state_convert_timestamp <= (uint32_t)bms_config.max_state_hold_time_ms) {
            tgt = TS_ACTIVE;
        }
        else {
            ctx->state_convert_timestamp = 0U;
            tgt = TS_INACTIVE;
        }
    }

    /* ④ 发布默认下一态/目标态（④5/⑥ 可能再改写 next_state） */
    out->next_state = cur;
    out->next_target = tgt;

    /* ⑤ manual AIR：打包值相等比较（不是逐位包含）；命中即 return，
     *    只置 manual 标志与位，**不写任何错误引脚/HV**。注意此分支在错误覆盖之后，
     *    故 error=1 时 manual 仍胜出（旧实现同序）。
     *    0b011(POS|NEG)→ACTIVE，0b110(NEG|PRE)→PRECHARGE，其余→INACTIVE。 */
    if(ctx->control_mode == TS_CONTROL_SERIAL_MANUAL_AIR) {
        uint8_t bits = ctx->manual_air_bits;
        if(bits == (AIR_BIT_NEG | AIR_BIT_POS)) {
            out->next_state = TS_ACTIVE;
        }
        else if(bits == (AIR_BIT_NEG | AIR_BIT_PRE)) {
            out->next_state = TS_PRECHARGE;
        }
        else {
            out->next_state = TS_INACTIVE;
        }
        out->manual_air = 1U;
        out->manual_air_bits = bits;
        return;
    }

    /* ⑥ 逐态 switch（无 default：非法态 next_state 保持 ④ 发布的 cur，与原逻辑一致） */
    switch(cur) {
    case TS_INACTIVE:
        out->next_state = fsm_inactive(ctx, in, out);
        break;
    case TS_ACTIVE:
        out->next_state = fsm_active(ctx, in, out);
        break;
    case TS_PRECHARGE:
        out->next_state = fsm_precharge(ctx, in, out);
        break;
    case TS_DISCHARGE:
        out->next_state = fsm_discharge(ctx, in, out);
        break;
    case TS_ERROR:
        out->next_state = fsm_error(ctx, in, out);
        break;
    case TS_CHARGE:
        out->next_state = fsm_charge(ctx, in, out);
        break;
    case TS_PRE_HOLD:
        out->next_state = fsm_pre_hold(ctx, in, out);
        break;
    default:
        break;
    }

    /* ⑦ 充电请求：只要"有充电意图且本拍未落故障"就回发请求帧，**不再要求已进入
     *    TS_CHARGE**（原实现在 fsm_charge() 里发，必须先走完预充）。
     *
     * 为什么必须提前发：预充达标门槛是 veh > 0.95·voltage_bat，而 vehicle 侧电压
     * 恰恰来自充电机；若充电机是"必须先收到本请求帧才输出电压"，原路径就是死锁
     * （预充 5s 超时 → ERROR，请求永不出）。上移到此处后，收到充电机 CCS 帧
     * （驱动层置 charge_mode → handle_bms_in 令 target=CHARGE）的那一拍就会回发。
     *
     * 保留两处原有护栏，二者都与改动前的可达集一致：
     *   1. 落故障不发——error 输入会把 cur 压成 TS_ERROR，故 next_state==TS_ERROR
     *      已完整覆盖。改动前 error 时状态进不了 fsm_charge，同样不发。
     *   2. SDC 断开不发——改动前能进 TS_CHARGE 意味着预充全程 SDC 闭合，且进态后
     *      一旦 !sdc_closed 立即转 DISCHARGE（当拍不发），故"请求只在 SDC 闭合时发"
     *      是原有不变量。放开它会变成"带电互锁已断开仍请求充电机出高压"。
     *
     * manual AIR 分支在 ⑤ 已 return，故手动调试模式不会发请求（与原行为一致）。 */
    if(in->sdc_closed && out->next_state != TS_ERROR &&
       out->next_target == TS_CHARGE) {
        out->send_charge_req = 1U;
    }
}
