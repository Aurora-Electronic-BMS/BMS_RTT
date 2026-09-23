#ifndef INC_SLAVE_MONITOR_H
#define INC_SLAVE_MONITOR_H

/* ==========================================================================
 * slave_monitor.h —— AFE 电芯采集的常量、数据结构与接口
 * 数据流与保护逻辑见 slave_monitor.c。
 * ========================================================================== */

#include "LTC68041.h"
#include "bms_config.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 保护阈值与级联片数都不再编译期固定：`total_ic` 在启动时由 bms_config 决定
 * （0x16 可改，掉电保持），IC 缓冲区按 BMS_MAX_IC 编到最大支持规模。 */

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
extern SlaveMessage slaves[BMS_MAX_IC];
extern uint16_t min_voltage;
extern uint16_t max_voltage;
extern int16_t max_temp;
extern int16_t min_temp;
extern int32_t Bat_Volt;
extern uint16_t cell_codes[BMS_MAX_IC][12];
extern int16_t temp[BMS_MAX_IC][6];

void slaves_init();
void slaves_check();
void slaves_clear_errors();
/* 把 bms_config 里的确认时长刷进四个监视器（bms_config_init/apply 时调用）。 */
void slaves_apply_config(void);
/* total_ic 变更后重排从机数组并重配 AFE 链（0x16 置配置后调用）。 */
void slaves_reinitialize_for_config(void);
void slaves_read();
int16_t temp_cal(uint16_t adc_value);
void PEC_volt_check(uint8_t pec, uint16_t cell[BMS_MAX_IC][12]);

/* 故障注入（逻辑验证用）：在 slave_monitor 层叠加覆盖 AFE 读数，
 * 不依赖 LTC6804 硬件/桩件。item 见 bms_io.h 的 BMS_FI_AFE_*，value=-1 撤销。 */
int slaves_inject(uint8_t item, int32_t value);

#endif
