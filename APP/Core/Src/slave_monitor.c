/* ==========================================================================
 * slave_monitor.c —— AFE（LTC6804）电芯电压/温度采集与保护
 *
 * 数据流：控制线程每 150ms 调 slaves_check() → slaves_read() 采一轮 AFE →
 * 遍历 TOTAL_IC 个 IC × 12 通道算 min/max 电压与温度、累加 Bat_Volt → 三/四个
 * ErrorMonitor 去抖 → ts_sm_set_error_source(TS_ERROR_SOURCE_SLAVE)。
 *
 * 当前 TOTAL_IC=12：12 IC × 12 通道 − 6 个空位（偶数 IC 的第 12 通道）= 138 芯。
 * 走桩件（LTC68041_stub.c）时每芯 3700mV，合计 Bat_Volt=510600mV，AUX 换算约 25℃。
 * 故障注入用 afe_inject_* 覆盖读数。
 *
 * 锁：电芯数据/温度被本线程写、遥测线程读，全程在 state_mtx 内完成。
 * ========================================================================== */
#include "slave_monitor.h"
#include "bms_io.h"
#include "shunt_monitor.h"
#include "LTC68041.h"
#include "main.h"
#include "can_custom.h"
#include "state_machine.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

SlaveMessage slaves[TOTAL_IC];   //从机数组变量
uint16_t min_voltage = 0xFFFF;
uint16_t max_voltage;
int16_t max_temp;
int16_t min_temp;
uint16_t min_voltage_error = 0xFFFF;
uint16_t max_voltage_error;
int16_t max_temp_error;
int16_t min_temp_error;
uint16_t cell_codes[TOTAL_IC][12];   //最近一轮原始电芯码（遥测上报用，已除 10 得 mV）
static uint16_t cell_buffer[TOTAL_IC][12];   //本拍读缓冲（经注入覆盖后拷入 cell_codes）
static uint16_t gpiocode[TOTAL_IC][6];       //AUX 原始值（温度）
int16_t temp[TOTAL_IC][6];                   //换算后温度（℃）
uint8_t pec = 0;
static int slave_comm_error = 0;
int32_t Bat_Volt;             //全包电压合计（mV），= 所有电芯之和

int slave_error = 0;          //从机聚合错误标志（对外/上报）

/* 四个保护的去抖：均 1000ms、锁存。 */
static ErrorMonitor slave_overvolt_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);
static ErrorMonitor slave_undervolt_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);
static ErrorMonitor slave_overtemp_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);
static ErrorMonitor slave_comm_monitor = ERROR_MONITOR_INIT(1000, ERROR_LATCH_ENABLE);

#if BMS_TEST_HOOKS
/* AFE 故障注入覆盖值：-1 = 不覆盖（用真实/桩件读数） */
static int32_t afe_inject_cell_raw = -1;
static int32_t afe_inject_aux_raw = -1;
static int32_t afe_inject_pec = -1;

/* 注入 AFE 覆盖值。cell_raw 覆盖全部电芯码，aux_raw 覆盖全部 AUX，pec 覆盖 PEC 结果。
 * 返回 0 成功、-1 未知 item。 */
int slaves_inject(uint8_t item, int32_t value) {
    bms_state_lock();
    switch(item) {
    case BMS_FI_AFE_CELL_RAW:
        afe_inject_cell_raw = value;
        break;
    case BMS_FI_AFE_AUX_RAW:
        afe_inject_aux_raw = value;
        break;
    case BMS_FI_AFE_PEC:
        afe_inject_pec = value;
        break;
    default:
        bms_state_unlock();
        return -1;
    }
    bms_state_unlock();
    return 0;
}
#endif

//从机初始化
void slaves_init() {
    for(int i = 0; i < TOTAL_IC; i++) {
        slaves[i].id = i + 1;
        slaves[i].error.kind = SLAVE_ERR_NONE;
        slaves[i].min_voltage = 0;
        slaves[i].max_voltage = 0;
        slaves[i].min_temp = 0;
        slaves[i].max_temp = 0;
    }
}

/* 串口清错：清通信错误标志与四个监视器（含锁存）。 */
void slaves_clear_errors() {
    bms_state_lock();
    slave_comm_error = 0;
    error_monitor_clear(&slave_overvolt_monitor);
    error_monitor_clear(&slave_undervolt_monitor);
    error_monitor_clear(&slave_overtemp_monitor);
    error_monitor_clear(&slave_comm_monitor);
    bms_state_unlock();
}

//启动AFE检测
/* 采一轮：ADC 转换电芯电压与 AUX，读回后应用注入覆盖，再拷入 cell_codes 并做 PEC 校验。 */
void slaves_read() {
    //wakeup_idle(TOTAL_IC);
    LTC6804_adcv();
    //HAL_Delay(3);
    pec = LTC6804_rdcv(0, TOTAL_IC, cell_buffer);
#if BMS_TEST_HOOKS
    if(afe_inject_pec >= 0) {
        pec = (uint8_t)afe_inject_pec;
    }
    if(afe_inject_cell_raw >= 0) {
        for(uint8_t i = 0; i < TOTAL_IC; i++) {
            for(uint8_t j = 0; j < 12U; j++) {
                cell_buffer[i][j] = (uint16_t)afe_inject_cell_raw;
            }
        }
    }
#endif
    /* cell_codes 由 PEC_volt_check 在校验通过时拷贝；PEC 坏时保留上一轮好数据。 */
    PEC_volt_check(pec, cell_buffer);
    //HAL_Delay(3);
    //wakeup_idle(TOTAL_IC);
    LTC6804_adax();
    //HAL_Delay(3);
    LTC6804_rdaux(0, TOTAL_IC, gpiocode);
#if BMS_TEST_HOOKS
    if(afe_inject_aux_raw >= 0) {
        for(uint8_t i = 0; i < TOTAL_IC; i++) {
            for(uint8_t k = 0; k < 6U; k++) {
                gpiocode[i][k] = (uint16_t)afe_inject_aux_raw;
            }
        }
    }
#endif
    //HAL_Delay(3);
}

//数据处理
/* 主巡检（150ms 一次）：采一轮 → 遍历算 min/max 电压温度与 Bat_Volt → 四条判定去抖上报。
 * 偶数编号 IC 的第 12 通道是空位（跳 No.24 cell 注释），累加/极值时跳过。 */
void slaves_check() {
    TSErrorKind kind = (TSErrorKind)0U;

    int32_t total_volt = 0;
    /* 全程持锁：电芯数据/温度由本线程写、遥测线程读，需一致快照 */
    bms_state_lock();
    slaves_read();
    max_voltage = 0x0000;
    min_voltage = 0xFFFF;
    max_temp = 0x0000;
    min_temp = 0x7FFF;
    for(int i = 0; i < TOTAL_IC; i++) {
        for(int j = 0; j < 12; j++) {     //Skip No.24 cell
            /* 偶数编号 IC(i=1,3,...,11) 的第 12 通道无电芯，跳过 */
            if((i == 1 || i == 3 || i == 5 || i == 7 || i == 9 || i == 11) && j == 11) {
                break;
            }

            /* 原始码 /10 折成 mV 后再比较 */
            if((cell_codes[i][j]/10) > max_voltage) {
                max_voltage = cell_codes[i][j]/10;
            }

            if((cell_codes[i][j]/10) < min_voltage) {
                min_voltage = cell_codes[i][j]/10;
            }

            total_volt += (uint32_t)cell_codes[i][j]/10;
        }

        /* 每个 IC 只用前 4 个 AUX 通道测温 */
        for(int k = 0; k < 4; k++) {
            temp[i][k] = temp_cal(gpiocode[i][k]);
            if(temp[i][k] > max_temp) {
                max_temp = temp[i][k];
            }
            if(temp[i][k] < min_temp) {
                min_temp = temp[i][k];
            }
        }
    }
    Bat_Volt = total_volt;

    /* 过压：任一芯 > MAX_CELL_VOLT(4200mV)。 */
    if(error_monitor_update(&slave_overvolt_monitor, max_voltage > MAX_CELL_VOLT)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SLAVE_OVERVOLT);
    }
    /* 欠压：任一芯 < MIN_CELL_VOLT(3200mV)。 */
    if(error_monitor_update(&slave_undervolt_monitor, min_voltage < MIN_CELL_VOLT)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SLAVE_UNDERVOLT);
    }
    /* 过温：任一芯 > MAX_CELL_TEMP(35℃)。 */
    if(error_monitor_update(&slave_overtemp_monitor, max_temp > MAX_CELL_TEMP)) {
        kind = (TSErrorKind)(kind | TS_ERROR_SLAVE_OVERTEMP);
    }
    /* AFE 内部通信错误：PEC 坏且有放电电流时置的 slave_comm_error。 */
    if(error_monitor_update(&slave_comm_monitor, slave_comm_error)) {
        kind = (TSErrorKind)(kind | TS_ERROR_INTERNAL_COMM_ERROR);
    }

    slave_error = (kind != 0U);
    ts_sm_set_error_source(TS_ERROR_SOURCE_SLAVE, slave_error, kind);
    bms_state_unlock();
}

//温度计算
/* NTC 分压反算温度：adc→电阻 Rt→开尔文→摄氏度。参数见 slave_monitor.h。
 * 分母用 NTC_R0（与 Rt 的 NTC_R0 同值，公式自洽）。参考版 BMS_V1.0 此处硬编码
 * 10000.0f，与 NTC_R0=100000 不自洽，会把温度整体压低几十度（过温保护失效），
 * 故本工程有意保留 /NTC_R0。 */
int16_t temp_cal(uint16_t adc_value) {
    float Rt = (adc_value / 10 * NTC_R0) / (Vref - adc_value / 10);
    float T = 1.0f/((1.0f/NTC_B) * logf(Rt/NTC_R0) + (1.0f/T0)) - 273.15f;
    return (int16_t)T;
}

/* PEC 校验结果处理：pec==255 表示读回数据损坏；仅在**有放电电流**（<-1A）时判定为
 * 真通信错误并点亮 STATUS_4（区分"真故障"与"静默期读噪声"）。pec 正常才把本拍缓冲
 * 拷入 cell_codes（PEC 坏时保留上一轮好数据），并清标志与引脚。 */
void PEC_volt_check(uint8_t pec, uint16_t cell[TOTAL_IC][12]) {
    if(pec == 255) {
        if(shunt_data.current < -PEC_CURR_THESHOLD) {
            slave_comm_error = 1;
            bms_out(STATUS_4_GPIO_Port, STATUS_4_Pin, GPIO_PIN_SET);
        }
    }
    else {
        memcpy(cell_codes, cell, sizeof(cell_codes));
        slave_comm_error = 0;
        bms_out(STATUS_4_GPIO_Port, STATUS_4_Pin, GPIO_PIN_RESET);
    }
}
