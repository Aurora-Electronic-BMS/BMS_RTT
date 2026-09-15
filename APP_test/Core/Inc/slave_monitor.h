#ifndef INC_SLAVE_MONITOR_H
#define INC_SLAVE_MONITOR_H

/* ==========================================================================
 * slave_monitor.h —— AFE 电芯采集的常量、数据结构与接口
 * 数据流与保护逻辑见 slave_monitor.c。
 * ========================================================================== */

#include "LTC68041.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

#define TOTAL_IC 10                  //级联的 LTC6804 个数
#define MAX_CELL_VOLT 4200           //mV
#define MIN_CELL_VOLT 3000           //mV
#define MAX_CELL_TEMP 50             //°C
#define MIN_CELL_TEMP 0              //°C
#define MAX_PEC_RECONN_TIME 2000     //ms
#define PEC_CURR_THESHOLD 1000       //mA：PEC 坏时，电流超过此值才判真通信错

/* NTC 分压反算温度用的参数（B 值、0℃ 电阻、T0 开尔文、参考电压 mV） */
#define NTC_B 4150.0f
#define NTC_R0 100000.0f
#define T0 298.15f
#define Vref 3000.0f

typedef enum {       //定义报错枚举量
    SLAVE_ERR_NONE,
    SLAVE_ERR_OT,    //过温
    SLAVE_ERR_UT,    //欠温
    SLAVE_ERR_OV,    //过压
    SLAVE_ERR_UV,    //欠压
    SLAVE_ERR_UNKNOWN,
} SlaveErrorKind;

typedef struct {     //定义报错结构体
    SlaveErrorKind kind;
    uint32_t data;
} SlaveError;

typedef struct {    //定义从机信息结构体
    uint8_t id;
    SlaveError error;
    uint16_t min_voltage;
    uint16_t max_voltage;
    int16_t min_temp;
    int16_t max_temp;
} SlaveMessage;

/* 全局汇总量：min/max_voltage 单位 mV，min/max_temp 单位 ℃，Bat_Volt 为全包合计 mV。
 * cell_codes/temp 供遥测逐芯上报。 */
extern SlaveMessage slaves[TOTAL_IC];
extern uint16_t min_voltage;
extern uint16_t max_voltage;
extern int16_t max_temp;
extern int16_t min_temp;
extern int32_t Bat_Volt;
extern uint16_t cell_codes[TOTAL_IC][12];
extern int16_t temp[TOTAL_IC][6];

void slaves_init();
void slaves_check();
void slaves_clear_errors();
void slaves_read();
int16_t temp_cal(uint16_t adc_value);
void PEC_volt_check(uint8_t pec, uint16_t cell[TOTAL_IC][12]);

/* 故障注入（逻辑验证用）：在 slave_monitor 层叠加覆盖 AFE 读数，
 * 不依赖 LTC6804 硬件/桩件。item 见 bms_io.h 的 BMS_FI_AFE_*，value=-1 撤销。 */
int slaves_inject(uint8_t item, int32_t value);

#endif
