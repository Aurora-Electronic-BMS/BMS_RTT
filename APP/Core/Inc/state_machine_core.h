#ifndef INC_STATE_MACHINE_CORE_H
#define INC_STATE_MACHINE_CORE_H

/* ==========================================================================
 * state_machine_core.h —— BMS 状态机的"纯决策内核"
 *
 * 为什么要把状态机拆成两层？
 * ------------------------------------------------------------------------
 * 原 state_machine.c 把三样东西混在一起：
 *   ① 决策态（文件级 static 的时间戳/control_mode/manual_air_bits）
 *   ② "算下一态"
 *   ③ "执行动作"（写引脚、发充电请求、阻塞延时）
 * 结果是：机器不可重入、状态散落各处、且完全没法脱离硬件驱动做单测。
 *
 * 拆法：
 *   本文件 + state_machine_core.c  = 纯决策层。只依赖 <stdint.h>/<stdbool.h>。
 *   state_machine.c                = 驱动层。负责取输入快照、调本内核、把结果落到引脚。
 *
 * 三条硬约束（改这个文件时必须守住）：
 *   1. 本文件不得出现任何平台头（HAL / RT-Thread / 具体芯片寄存器）。
 *      一旦出现，tools/fsm_test 的宿主编译会立刻失败——这是刻意的守门人。
 *   2. ts_fsm_step() 除读写 ctx/out 外不得有任何副作用；时间一律由 in->now 传入，
 *      不得调用 HAL_GetTick() 之类。
 *   3. 行为必须与原实现逐拍等价：每条迁移条件、每个引脚电平、每个时间阈值都照抄。
 *      这是安全相关模块，不允许"顺手修"。
 *
 * 【原实现对照】约定
 * ------------------------------------------------------------------------
 * 本文件与 state_machine_core.c 里的注释会写"原 ts_sm_xxx()"，指的是**重构前**那版
 * state_machine.c 里的同名函数（该文件已被本设计取代）。对照以"函数名"为准，因为
 * 函数名是稳定锚点；重构前的行号仅供追溯，不再逐处标注。
 * 完整的映射关系见 state_machine_core.c 文件头的对照表。
 * ========================================================================== */

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * 时间常量
 * 数值一律照抄原 state_machine.h，一个都不能动——它们直接决定迁移边界在哪一拍发生。
 * -------------------------------------------------------------------------- */
#define MIN_VEHICLE_SIDE_VOLTAGE 150    //mV：预充判定里"车辆侧电压至少要有这么多"的下限
                                        //    仅用于挡住"车辆侧根本没接线（~0V）"的噪声
#define DISCHARGE_DURATION 5000         //ms：DISCHARGE 态驻留时长；也是 ERROR 恢复时
                                        //    "回 DISCHARGE 还是回 INACTIVE"的分水岭
#define NO_ERROR_TIME 1000              //ms：ERROR 态下错误消失后，需连续无错多久才允许恢复
#define AIR_CLOSE_WAIT 10               //ms：同一继电器两次闭合之间的最小间隔（消抖/防拍合）
#define PRECHARGE_95_WAIT_TIME 5000     //ms：预充超时窗口。5s 内车辆侧到不了 0.95·vbat 就报错
#define PRE_HOLD_TIME 200               //ms：PRE_HOLD 态驻留时长（等继电器吸合稳定再合主接触器）
#define MAX_STATE_HOLD_TIME 500         //ms：POWER_UP 落下后，ACTIVE 还能维持多久（延时下电）
#define DRIVE_MODE_TIMEOUT 2000         //ms：行车模式心跳超时；本内核不读它，仅驱动层用
#define SERIAL_CONTROL_TIMEOUT 1000U    //ms：串口控制（target/manual AIR）失联超时后交还 GPIO
#define CHARGE_MAX_REVERSE_CURRENT 300  //mA：CHARGE 态允许的反向(放电)电流上限。
                                        //    按符号约定充电为正，故正常应 ≥0；只有反向电流
                                        //    超过这个量才判故障，给分流器零点漂移与纹波留余量

/* --------------------------------------------------------------------------
 * 错误源掩码
 * ts_sm_set_error_source(flag, ...) 的 flag：每个"来源"对应 ts_error_kind 里的一组位。
 * 一个来源置位/清零时，只动它名下那几位，不碰别人——多来源可同时报错。
 * -------------------------------------------------------------------------- */
#define TS_ERROR_SOURCE_SLAVE (1 << 0)         //从机/电芯来源（slave_monitor.c 调用）
#define TS_ERROR_SOURCE_SHUNT (1 << 1)         //分流器来源（shunt_monitor.c 调用）
#define TS_ERROR_SOURCE_STATE_MACHINE (1 << 2) //状态机自身来源（目前只有预充超时）

/* --------------------------------------------------------------------------
 * AIR 位（manual_air_bits 的打包值）
 * 注意是"位"不是"枚举序号"，manual 分支用**打包后的整值做 == 比较**：
 *   0b011 (POS|NEG) → ACTIVE
 *   0b110 (NEG|PRE) → PRECHARGE
 *   其余           → INACTIVE
 * -------------------------------------------------------------------------- */
#define AIR_BIT_POS (1 << 0)
#define AIR_BIT_NEG (1 << 1)
#define AIR_BIT_PRE (1 << 2)

/* --------------------------------------------------------------------------
 * 状态枚举
 * 数值 0~6 直接进遥测载荷与 [SM]/[ST] 文本（sm=%u），改动即破坏上位机解析。
 * -------------------------------------------------------------------------- */
typedef enum {
    TS_INACTIVE,      //0x00 空闲：AIR 全断，允许预充
    TS_ACTIVE,        //0x01 运行：主接触器闭合，HV_ACTIVE 置位
    TS_PRECHARGE,     //0x02 预充：NEG_AIR + PRECHARGE 闭合，等车辆侧电压爬升
    TS_DISCHARGE,     //0x03 放电：AIR 全断后的驻留期，等母线泄放
    TS_ERROR,         //0x04 故障：BMS_ERROR 置位，HV_ACTIVE 复位
    TS_PRE_HOLD,      //0x05 预充保持：等待继电器吸合稳定
    TS_CHARGE,        //0x06 充电：与 ACTIVE 类似但发充电请求
} TSState;

/* --------------------------------------------------------------------------
 * 错误种类位图
 * TS_ERROR_NONE 是"无错"哨兵（bit0）；只要 error 非 0，bit0 就被清掉。
 * 详见 state_machine.c 的 ts_sm_set_error_source()。
 * -------------------------------------------------------------------------- */
typedef enum {
    TS_ERROR_NONE = 1U << 0,                 //无错哨兵：error==0 时 ts_error_kind 才是这个值
    TS_ERROR_SLAVE_OVERVOLT = 1U << 1,       //电芯过压
    TS_ERROR_SLAVE_UNDERVOLT = 1U << 2,      //电芯欠压
    TS_ERROR_SLAVE_OVERTEMP = 1U << 3,       //电芯过温
    TS_ERROR_SHUNT_TIMEOUT = 1U << 4,        //分流器断流（CAN 无更新超时）
    TS_ERROR_SHUNT_OVERCURRENT = 1U << 5,    //分流器过流
    TS_ERROR_SHUNT_OVERTEMP = 1U << 6,       //分流器/母线过温
    TS_ERROR_VOLTAGE_MISMATCH = 1U << 7,     //电池侧与分流器电压失配
    TS_ERROR_INTERNAL_COMM_ERROR = 1U << 8,  //AFE 内部通信错误（PEC 坏 + 有放电电流）
    TS_ERROR_PRECHARGE_TIMEOUT = 1U << 9,    //预充超时（由状态机自己产生）
} TSErrorKind;

/* --------------------------------------------------------------------------
 * AIR 继电器编号（ts_sm_ctrl_AIR_pwr 的 relay 参数）
 * -------------------------------------------------------------------------- */
typedef enum {
    AIR_POS,   //正极主接触器
    AIR_NEG,   //负极主接触器
    AIR_PRE,   //预充接触器（串预充电阻）
} AIR;

/* --------------------------------------------------------------------------
 * 控制模式：谁在决定 target_state
 * -------------------------------------------------------------------------- */
typedef enum {
    TS_CONTROL_GPIO = 0,              //本地逻辑：由 POWER_UP/CHARGE 输入决定（默认）
    TS_CONTROL_SERIAL_TARGET = 1,     //上位机指定目标态（0x10 命令）
    TS_CONTROL_SERIAL_MANUAL_AIR = 2, //上位机直接手动吸合 AIR（0x11 命令，调试用）
} TSControlMode;

/* --------------------------------------------------------------------------
 * 引脚写掩码
 * 关键设计：内核不返回"目标态对应的完整引脚值"，而是返回"本拍允许写哪几个引脚"。
 *
 * 为什么必须这样？因为原实现里存在**部分引脚写**：
 *   - 预充超时进入 ERROR（原 ts_sm_precharge_timeout）：这拍只把 NERROR 拉低，
 *     BMS_ERROR 那拍**根本不动**（保持 0）。
 *   - CHARGE 态且 current<-300 转 ERROR：这拍写了 HV_ACTIVE=SET 后就直接返回，
 *     并没有把它复位。
 * 如果驱动层按"目标态"整体 apply 引脚，上面两处的电平就会提前跳变，
 * 而 HV_ACTIVE 是**外部可观测**的（遥测载荷里有、还驱动 LED1）。
 * 所以内核返回掩码 + 电平，驱动层逐位写，才能复现"本拍只改了哪几个引脚"。
 * -------------------------------------------------------------------------- */
#define TS_PIN_NERROR    (1u << 0)  //BMS_NERROR（"无故障"指示，低有效语义上是"正常"）
#define TS_PIN_BMS_ERROR (1u << 1)  //BMS_ERROR（故障指示。ERROR 态置位、恢复时复位）
#define TS_PIN_HV_ACTIVE (1u << 2)  //HV_ACTIVE（高压已接通；遥测 + LED1 都在读它）

/* --------------------------------------------------------------------------
 * TsFsmCtx —— 全部决策态
 * 原实现里这些是 state_machine.c 的文件级 static，机器因此不可重入。收进结构体后：
 *   - 状态集中一处，可整体初始化/整机复位
 *   - 同一份内核能被宿主单测反复喂不同 ctx 跑
 *
 * 谁读写：
 *   控制线程在锁内跑 ts_fsm_step() 读写绝大部分字段；
 *   遥测线程的串口 setter（ts_sm_serial_*）只写 control_mode /
 *   manual_air_bits / serial_control_timestamp（同样持锁）；
 *   CAN RX 线程只写 last_vcu_can_msg（原实现也无锁，保持一致）。
 *   air_* 三个状态**只被 ctrl 线程在锁外读写**（AIR 执行留在驱动层），setter 不碰，安全。
 *
 * 刻意**不**放进 ctx 的字段：
 *   current_state / target_state / error —— 它们经 TsFsmInputs/TsFsmOut 流入流出，
 *   让全局 ts_state 保持"唯一权威"，跨模块（遥测、CAN、bms_io）的读写语义不变。
 *   drive_mode —— 没有任何迁移读它（只有 error_monitor_update 用），留在 can_custom.c。
 * -------------------------------------------------------------------------- */
typedef struct {
    TSControlMode control_mode;                  //当前控制模式，默认 TS_CONTROL_GPIO(0)
    uint8_t  manual_air_bits;                    //手动 AIR 的打包位（AIR_BIT_*）
    uint32_t serial_control_timestamp;           //最近一次串口控制/心跳的时刻（判失联）
    uint32_t precharge_start_timestamp;          //进入 PRECHARGE 的时刻（判预充超时）
    uint32_t precharge_95_finished_timestamp;    //车辆侧达到 0.95·vbat 的时刻（PRE_HOLD 计时起点）
    uint32_t discharge_begin_timestamp;          //进入 DISCHARGE 的时刻（判 5000ms 驻留）
    uint32_t state_convert_timestamp;            //POWER_UP 落下后 ACTIVE 的保持计时起点
                                                 //（哨兵 0 表示"还没开始计时"）
    uint32_t no_error_start;                     //ERROR 态下错误消失的起点
                                                 //（哨兵 0 表示"还没进入恢复观察期"）
    uint32_t last_vcu_can_msg;                   //最近一帧整车 CAN（drive_mode 超时用）
    int      air_pos_state;                      //三个继电器"当前已命令的电平"，
    int      air_neg_state;                      //  用于检测跳变并施加 AIR_CLOSE_WAIT
    int      air_pre_state;                      //  （原 ts_sm_ctrl_AIR_pwr 内的 static）
    uint32_t air_last_close_timestamp;            //最近一次闭合动作的时刻（原 AIR_close_wait 内 static）
} TsFsmCtx;

/* --------------------------------------------------------------------------
 * TsFsmInputs —— 每拍输入快照
 * 全部由驱动层在锁内一次性取好，保证同一拍看到的是**一致**的数据
 * （原实现是全无锁的，边用边读，CAN RX 线程正在写 shunt_data 时会读到撕裂值）。
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t now;           //本拍的时间基准（驱动层取自 HAL_GetTick）
    TSState  current_state; //ts_state.current_state 的快照
    TSState  target_state;  //ts_state.target_state 的快照
    uint8_t  error;         //布尔：ts_state.error != 0（有**任何**错误源置位）
    uint8_t  power_up;      //布尔：POWER_UP 数字输入
    uint8_t  sdc_closed;    //布尔：SDC（安全断开回路）闭合
    uint8_t  charge_mode;   //0/1：整车请求充电（来自充电机 CCS 帧）
    int32_t  voltage_bat;   //mV：电池侧电压（分流器上报）
    int32_t  voltage_veh;   //mV：车辆侧电压（分流器上报）
    int32_t  current;       //mA：有符号，放电为正还是充电为正由分流器定义
} TsFsmInputs;

/* --------------------------------------------------------------------------
 * TsPinWrites —— "本拍写了哪些引脚、各写什么电平"
 * write 是 TS_PIN_* 的位掩码；对应位为 1 才表示"这一拍动了这个引脚"。
 * 未置位的引脚保持上一拍电平不动——这正是复现部分写语义的地方。
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t write;      //本拍允许写的位（TS_PIN_* 组合）
    uint8_t nerror;     //write 含 TS_PIN_NERROR 时的目标电平（1=SET）
    uint8_t error;      //write 含 TS_PIN_BMS_ERROR 时的目标电平
    uint8_t hv_active;  //write 含 TS_PIN_HV_ACTIVE 时的目标电平
} TsPinWrites;

/* --------------------------------------------------------------------------
 * TsFsmOut —— 本拍的决策结果
 * 纯数据，不含任何动作。驱动层拿到后：
 *   1) 把 next_state/next_target 发布回 ts_state
 *   2) 锁外按 pins.write 逐位写引脚
 *   3) 锁外执行 send_charge_req / assert_precharge_timeout / manual AIR
 * -------------------------------------------------------------------------- */
typedef struct {
    TSState    next_state;               //下一拍的状态
    TSState    next_target;              //下一拍的目标态（可能被 handle_bms_in 重算）
    TsPinWrites pins;                    //本拍该写哪几个引脚
    uint8_t    manual_air;               //1 = 走 manual AIR 分支：由 manual_air_bits 直接驱
                                         //    动继电器，且**不写**错误引脚/HV
    uint8_t    manual_air_bits;          //manual 分支要吸合的 AIR 位
    uint8_t    send_charge_req;          //1 = 驱动层调 can_send_charge_req()。
                                         //    条件：target==CHARGE 且未落故障且 SDC 闭合。
                                         //    **不要求已进入 TS_CHARGE**——预充达标要靠
                                         //    充电机给的车辆侧电压，故请求必须先于预充发出
    uint8_t    assert_precharge_timeout; //1 = 驱动层调 ts_sm_set_error_source(
                                         //        TS_ERROR_SOURCE_STATE_MACHINE, 1,
                                         //        TS_ERROR_PRECHARGE_TIMEOUT)
                                         //    之所以做成输出位而不是在内核里直接调用：
                                         //    内核不能有副作用，且该调用会取锁。
} TsFsmOut;

/* --------------------------------------------------------------------------
 * 对外接口
 * -------------------------------------------------------------------------- */

/* 复位 ctx 到"刚上电"的等价状态（等价于原 static 的全零初始化 + 默认 GPIO 模式）。
 * 必须在创建线程/首次 step 之前调用一次。 */
void ts_fsm_init(TsFsmCtx *ctx);

/* 串口控制失联超时复位：串口模式下超过 SERIAL_CONTROL_TIMEOUT 没消息 → 交还 GPIO。
 * **必须在 ts_fsm_step() 之前调用**：驱动层要靠它先定下本拍的 control_mode，
 * 才能判断"本拍该不该跑 charge_cmd_check()"（该判断依赖超时后的 control_mode）。
 * 单独拆出来是为了让驱动层能插进这个中间步骤，同时保持函数本身仍然是纯的。 */
void ts_fsm_apply_serial_timeout(TsFsmCtx *ctx, uint32_t now);

/* 推进一拍。除读写 ctx/out 外无副作用。 */
void ts_fsm_step(TsFsmCtx *ctx, const TsFsmInputs *in, TsFsmOut *out);

#endif
