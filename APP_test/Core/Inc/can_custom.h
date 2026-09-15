#ifndef INC_CAN_H
#define INC_CAN_H

/* ==========================================================================
 * can_custom.h —— CAN 报文 ID、收发接口与注入入口
 * 双总线布局与 ISR→环形缓冲→线程 的收发模型见 can_custom.c 头部。
 * ========================================================================== */

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_can.h"
#include "stm32f4xx_hal_def.h"
#include "state_machine.h"

/* BMS 状态帧（CAN2 发出） */
#define CAN_ID_BMS_STATUS1 0x301
#define CAN_ID_BMS_STATUS2 0x302
#define CAN_ID_BMS_STATUS3 0x303
/* 整车 → BMS 输入帧（CAN1 收）：请求充电 */
#define CAN_ID_BMS_IN 0x700
/* 分流器数据段（CAN1 收，0x52x） */
#define CAN_ID_SHUNT_BASE 0x520
#define CAN_ID_SHUNT_CURRENT 0x521
#define CAN_ID_SHUNT_VOLTAGE_1 0x522
#define CAN_ID_SHUNT_VOLTAGE_2 0x523
#define CAN_ID_SHUNT_VOLTAGE_3 0x524
#define CAN_ID_SHUNT_BUSBAR_TEMP 0x525
#define CAN_ID_SHUNT_POWER 0x526
#define CAN_ID_SHUNT_CURRENT_COUNTER 0x527
#define CAN_ID_SHUNT_ENERGY_COUNTER 0x528
/* 充电机 CCS（CAN2，扩展帧，收）；BMS 充电请求（CAN2，发） */
#define CAN_ID_CHARGER_CCS 0x18FF50E5
#define CAN_ID_CHARGE_REQ 0x1806E5F4
/* 整车行车使能（CAN1 收） */
#define CAN_ID_DRIVE_ENABLE 0x188

/* charge_mode 在无新 BMS_IN 帧后多久自动过期（ms） */
#define CHARGE_CMD_TIMEOUT 1000

/* CAN RX 环形缓冲：ISR 入队、线程出队分发 */
#define BMS_CAN_RING_SIZE 64
typedef struct {
    uint8_t  bus;     /* 1=CAN1, 2=CAN2 */
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} bms_can_msg_t;

extern uint8_t charge_mode;           /* 整车请求充电（有 1s 过期） */
extern volatile uint8_t drive_mode;   /* 行车模式（有独立心跳看门狗） */

HAL_StatusTypeDef can_init(void);
HAL_StatusTypeDef can_send_status();
HAL_StatusTypeDef can_send_volt();
HAL_StatusTypeDef can_send_charge_req();

/* 环形缓冲出队 / 分发 / 丢帧计数 */
int  can_custom_msg_pop(bms_can_msg_t *out);
void can_custom_dispatch(const bms_can_msg_t *m);
uint32_t can_custom_drop_count(void);

/* 回环自测：周期性把合成帧发到回环总线，用来验证 RX→过滤→分发 全链路 */
void can_custom_selftest(int enable);
void can_custom_selftest_tick(void);

//HAL_StatusTypeDef can_send_error(TSErrorKind kind, uint8_t device, int16_t value);
//HAL_StatusTypeDef can_send_error_state();

/* charge_mode 过期检查（由状态机驱动层在取快照前调用） */
void charge_cmd_check();

/* 故障注入（逻辑验证用）：模拟收到充电机 CCS 帧，置 charge_mode 并刷新时间戳。
 * charge_mode 1s 后由 charge_cmd_check 自动过期，需周期重发。返回 0 成功。 */
int charge_inject(int32_t mode);
#endif
