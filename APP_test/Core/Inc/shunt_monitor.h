#ifndef INC_SHUNT_MONITOR_H
#define INC_SHUNT_MONITOR_H

/* ==========================================================================
 * shunt_monitor.h —— 分流器保护阈值、数据结构与接口
 * 行为说明见 shunt_monitor.c。
 * ========================================================================== */

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* 保护阈值。注意过流分方向：放电允许更大（200A），充电限制很严（15A）。 */
#define SHUNT_TIMEOUT 500                       //ms
#define SHUNT_OVERCURRENT_DISCHARGE 200000      //mA
#define SHUNT_OVERCURRENT_CHARGE 15000          //mA
#define SHUNT_OVERCURREMT_REGEN 25000           //mA
#define SHUNT_OVERTEMP 1000                     //1/10 ˚C

/* 分流器上报数据（全部来自 CAN1 0x52x 段）。current 有符号，正负号定义由分流器决定。
 * last_current_message==0 表示还没收到过电流帧，电流累计从第二帧起才计算。 */
typedef struct {
    int32_t current;              //mA
    int32_t voltage_bat;          //mV 电池侧
    int32_t voltage_veh;          //mV 车辆侧
    int32_t voltage_x;            //mV 第 3 路电压（未参与保护）
    int32_t busbartemp;           //0.1℃ 母线温度
    int32_t power;                //W
    int32_t energy;               //Wh
    float current_counter;        //mAs 级电流累计（手动积分）
    uint32_t last_message;        //最近一帧任意分流器帧的时刻（判超时）
    uint32_t last_current_message;//最近一帧电流帧的时刻（算 dt）
} ShuntData;

extern ShuntData shunt_data;
extern int shunt_error;

void shunt_init();
void shunt_check();
void shunt_clear_errors();
void shunt_handle_can_message(uint16_t id, const uint8_t *data);

/* 故障注入（逻辑验证用）：直接改写 shunt_data 字段并刷新 last_message。
 * item 见 bms_io.h 的 BMS_FI_SHUNT_*。返回 0 成功，-1 未知 item。 */
int shunt_inject(uint8_t item, int32_t value);

#endif
