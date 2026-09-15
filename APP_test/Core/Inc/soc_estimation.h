#ifndef INC_SOC_ESTIMATION_H
#define INC_SOC_ESTIMATION_H

/* ==========================================================================
 * soc_estimation.h —— SOC（荷电状态）估算：OCV 查表 + 安时积分
 *
 * 两路信息融合：
 *   - 静置（|电流| < NO_CURRENT_THRESHOLD 持续 NO_CURRENT_TIME）时，用最低单体
 *     电压查 OCV-SOC 表，作为"真值"校准。
 *   - 有电流时，对分流器累计电量 current_counter 做安时积分，在静置起点给出的
 *     基准上推算 SOC。
 *
 * 符号约定（与 shunt_monitor 一致）：**充电为正、放电为负**。因此安时积分
 * 是 current_soc = 基准 + ΔQ/Q_full*100；充电 ΔQ>0 → SOC 上升。
 * 输出 current_soc 已钳位到 [0,100]（%）。行为细节见 soc_estimation.c。
 * ========================================================================== */

#include <stdint.h>

#define NO_CURRENT_THRESHOLD 200       /* mA：|电流| 低于此值视为静置 */
#define NO_CURRENT_TIME 100000         /* ms：静置超过此时长改用 OCV 校准 */
#define CELL_CAPACITY 50760000         /* mAs：安时积分满量程（≈14.1 Ah） */

typedef struct {
    uint16_t ocv;                      /* mV，单体开路电压 */
    float    soc;                      /* % */
} ocv_soc;

extern float current_soc;              /* %，钳位到 [0,100] */

void soc_init(void);
void soc_update(void);

#endif /* INC_SOC_ESTIMATION_H */
