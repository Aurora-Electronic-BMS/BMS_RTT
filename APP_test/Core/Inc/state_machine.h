#ifndef INC_STATE_MACHINE_H
#define INC_STATE_MACHINE_H

/* ==========================================================================
 * state_machine.h —— BMS 状态机的**驱动层/公开接口**
 *
 * 分层：state_machine_core.{h,c} 是平台无关的纯决策内核；本文件 + state_machine.c
 * 是驱动层，负责取输入快照、调内核、把结果落到引脚/CAN/串口。
 *
 * 本文件曾经承载全部枚举与时间常量，重构后那些已移入 state_machine_core.h；
 * 这里只保留：HAL 依赖、充电上限常量、错误监视器（ErrorMonitor）、全局状态句柄，
 * 以及驱动层对外 API。7 个现有 includer 因此零改动即可继续编过。
 * ========================================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

#include "state_machine_core.h"   /* 转发 TSState/TSErrorKind/AIR/TSControlMode/时间常量 */

/* 充电请求的硬件上限。can_custom.h 等模块直接取这两个宏，故必须留在本头。 */
#define MAX_CHARGE_VOLT 5800            //0.1V
#define MAX_CHARGE_CURR 30              //0.1A

/* ERROR_LATCH_ENABLE 为 1 时，ErrorMonitor 一旦 confirm 就锁存，需显式 clear 才复位；
 * 为 0 时错误消失即自动复位。当前取 1（安全相关：故障需人工确认）。 */
#define ERROR_LATCH_ENABLE 1
/* 错误监视器初始化器：指定去抖时长与是否锁存，其余字段(C风格)零初始化。 */
#define ERROR_MONITOR_INIT(delay_value, latch_value) { .delay_ms = (delay_value), .latch = (latch_value) }

/* --------------------------------------------------------------------------
 * TSStateHandle —— 状态机的"唯一权威"全局状态
 * current_state/target_state/error 刻意**不入** TsFsmCtx：它们经 TsFsmInputs/TsFsmOut
 * 流入流出内核，仍由全局唯一副本持有，跨模块（遥测/CAN/bms_io）读写语义不变。
 * error 是"任一来源置位"的聚合位（非 TSErrorKind 细分；细分见 ts_error_kind）。
 * -------------------------------------------------------------------------- */
typedef struct {
    TSState current_state;
    TSState target_state;
    uint32_t error;
} TSStateHandle;

/* --------------------------------------------------------------------------
 * ErrorMonitor —— 通用错误去抖器（过压/欠压/过温/断流/失配等共用）
 * pending: 条件出现、正在计时但未确认；confirm: 已确认（对外报错）；
 * latch: 是否锁存（配合 ERROR_LATCH_ENABLE）。
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t delay_ms;
    uint32_t pending_start;
    bool pending;
    bool confirm;
    bool latch;
} ErrorMonitor;

extern TSStateHandle ts_state;   //全局状态机状态（唯一权威）
extern uint16_t ts_error_kind;   //细分错误位图（TSErrorKind 组合；0 时为 TS_ERROR_NONE）
extern int slave_error;          //从机聚合错误标志（slave_monitor 维护）

/* ---- 生命周期 ---- */
void ts_sm_init();
void ts_sm_update();

/* ---- 驱动层：引脚/AIR 执行、CAN 入口、串口 setter（决策在 state_machine_core）---- */
void ts_sm_set_AIR_positions(TSState state);         //按目标态设置三个继电器电平
void ts_sm_ctrl_AIR_pwr(AIR relay, int closed_cmd);  //单个继电器控制（含防拍合等待）
void ts_sm_AIR_close_wait(int *AIR_state, int AIR_cmd); //闭合沿消抖，必要时阻塞等待
void charge_handle_can_message(const uint8_t *data); //充电机 CAN 入口（当前留空）
void sm_handle_can_message(const uint8_t *data);     //整车 CAN 入口（drive_mode 心跳）
void drive_mode_timeout_check();                     //行车模式心跳超时复位

/* 设置某"来源"的错误位。flag 取 TS_ERROR_SOURCE_*；state 非 0 置位、为 0 清零；
 * kind 是该来源下的细分错误（多来源可同时报错，互不干扰）。 */
void ts_sm_set_error_source(uint32_t flag, int state, TSErrorKind kind);

/* 串口控制接口：指定目标态 / 手动 AIR / 清错 / 心跳 / 查询当前模式与 AIR 实际电平 */
int ts_sm_serial_set_target(TSState state);
int ts_sm_serial_set_manual_air(int enable, uint8_t air_bits);
int ts_sm_serial_clear_errors(void);
void ts_sm_serial_heartbeat(void);
TSControlMode ts_sm_get_control_mode(void);
uint8_t ts_sm_get_air_bits(void);

/* ---- 错误去抖器的更新/清除 ---- */
bool error_monitor_update(ErrorMonitor *monitor, bool exist);
void error_monitor_clear(ErrorMonitor *monitor);

#endif
