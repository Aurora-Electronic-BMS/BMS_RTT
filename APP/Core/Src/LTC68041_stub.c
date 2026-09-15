/* LTC68041_stub.c —— LTC6804(电芯采集 AFE) 桩件
 *
 * 本工程不带 LTC6804 硬件，为让 slave_monitor.c 的完整逻辑(逐芯遍历、min/max、
 * 温度换算、Bat_Volt 累加、PEC/通讯错误判定)能在 RT-Thread 上跑通，用本桩件
 * 顶替真实驱动：
 *   - 电芯电压固定 37000 (即 3700.0 mV，落在 3000~4200 mV 正常区间)
 *   - AUX(GPIO) 原始值固定 15000，经 temp_cal() 反算出 25 °C
 *   - PEC 返回 0(正常)，不触发通讯错误
 * 待真实 AFE 硬件到位后，用 LTC68041.c 替换本文件即可，上层无需改动。
 */
#include "LTC68041.h"

#define STUB_CELL_MV 37000U   /* 3700.0 mV */
#define STUB_AUX_RAW 15000U   /* temp_cal(15000) ≈ 25 °C */

void LTC6804_initialize(void) { }

void LTC6804_adcv(void) { }

void LTC6804_adax(void) { }

void LTC6804_clrcell(void) { }

void LTC6804_clraux(void) { }

uint8_t LTC6804_rdcv(uint8_t reg, uint8_t total_ic, uint16_t cell_codes[][12]) {
    (void)reg;
    for (uint8_t i = 0U; i < total_ic; i++) {
        for (uint8_t j = 0U; j < 12U; j++) {
            cell_codes[i][j] = STUB_CELL_MV;
        }
    }
    return 0U;   /* PEC 正常 */
}

int8_t LTC6804_rdaux(uint8_t reg, uint8_t nIC, uint16_t aux_codes[][6]) {
    (void)reg;
    for (uint8_t i = 0U; i < nIC; i++) {
        for (uint8_t k = 0U; k < 6U; k++) {
            aux_codes[i][k] = STUB_AUX_RAW;
        }
    }
    return 0;
}
