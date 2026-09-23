#ifndef INC_SOC_ESTIMATION_H
#define INC_SOC_ESTIMATION_H

/* ==========================================================================
 * soc_estimation.h —— SOC（荷电状态）估算：OCV 查表 + 安时积分 + 掉电保持
 *
 * 与 BMS_V1.0 新版（610b476）一致：
 *   - 上电从 flash 读回上次持久化的 SOC（soc_storage_load，Sector 10）。
 *   - 有电流时对分流器累计电量 current_counter 做安时积分；充电为正、放电为负。
 *   - SOC 每变化 ≥1%（或到 0/100、或退出行车模式并断开高压）写回 flash。
 *   - 串口 0x17 可用当前 min_voltage 走 OCV 表重定 SOC 并立即持久化。
 *   - charge_finish 置 1 时下一拍把 SOC 校准为 100% 并持久化（充满标志）。
 *
 * 输出 current_soc 已钳位并四舍五入到 0.01%，范围 [0,100]。
 * ========================================================================== */

#include <stdbool.h>
#include <stdint.h>

/* 单体标称容量，单位为 mAs。 */
#define CELL_CAPACITY 14
#define CELL_CAPACITY_MAS (CELL_CAPACITY * 1000 * 3600.0f)

typedef struct {
    uint16_t ocv;
    float soc;
} ocv_soc;

extern float current_soc;

/* 置为 1 后，SOC 在主循环中被校准为 100.00% 并持久化。 */
extern volatile uint8_t charge_finish;

void soc_init(void);
void soc_update(void);
bool soc_update_from_ocv(uint16_t ocv_mV);

#endif
