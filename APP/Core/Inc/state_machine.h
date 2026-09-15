#ifndef INC_STATE_MACHINE_H
#define INC_STATE_MACHINE_H

/* ==========================================================================
 * state_machine.h —— BMS 状态机【驱动层】接口
 *
 * 分层：state_machine_core.{h,c} 是平台无关的纯决策内核（只含 <stdint.h>/<stdbool.h>），
 * 定义 TSState / TSErrorKind / AIR / TSControlMode / 时间常量 / TsFsm*；
 * 本头文件只声明"接到硬件上"的那一层：全局权威状态 ts_state、错误去抖监视器，
 * 以及串口 setter。实现见 state_machine.c。
 * ========================================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"
#include "state_machine_core.h"

/* 充电请求帧（CAN_ID_CHARGE_REQ）里带的上限 */
#define MAX_CHARGE_VOLT 5800            //0.1V
#define MAX_CHARGE_CURR 30              //0.1A

/* 故障去抖监视器：确认后是否锁存（须显式清错） */
#define ERROR_LATCH_ENABLE 1
#define ERROR_MONITOR_INIT(delay_value, latch_value) { .delay_ms = (delay_value), .latch = (latch_value) }

/* 全局状态机的"唯一权威"副本：跨模块（遥测/CAN/bms_io）读它 */
typedef struct {
    TSState current_state;
    TSState target_state;
    uint32_t error;
} TSStateHandle;

/* 通用错误去抖器：条件持续 delay_ms 才 confirm；latch 为真则确认后不自复位 */
typedef struct {
    uint32_t delay_ms;
    uint32_t pending_start;
    bool pending;
    bool confirm;
    bool latch;
} ErrorMonitor;

extern TSStateHandle ts_state;
extern uint16_t ts_error_kind;
extern int slave_error;

void ts_sm_init();
void ts_sm_update();

void ts_sm_set_AIR_positions(TSState state);
void ts_sm_ctrl_AIR_pwr(AIR relay, int closed_cmd);
void ts_sm_AIR_close_wait(int *AIR_state, int AIR_cmd);
void charge_handle_can_message(const uint8_t *data);
void sm_handle_can_message(const uint8_t *data);
void drive_mode_timeout_check();
void ts_sm_set_error_source(uint32_t flag, int state, TSErrorKind kind);
int ts_sm_serial_set_target(TSState state);
int ts_sm_serial_set_manual_air(int enable, uint8_t air_bits);
int ts_sm_serial_clear_errors(void);
void ts_sm_serial_heartbeat(void);
TSControlMode ts_sm_get_control_mode(void);
uint8_t ts_sm_get_air_bits(void);

bool error_monitor_update(ErrorMonitor *monitor, bool exist);
void error_monitor_clear(ErrorMonitor *monitor);

#endif
