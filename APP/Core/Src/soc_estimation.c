/* ==========================================================================
 * soc_estimation.c —— SOC 估算：OCV 查表 + 安时积分
 *
 * 调用约定：soc_update() 读 shunt_data / min_voltage 并写 current_soc，由控制
 * 线程在 bms_state_lock() 内调用（current_soc 会被遥测线程读取）。
 *
 * 相对 BMS_V1.0 原实现的修正（原版零调用，缺陷从未暴露）：
 *   1. 判静置原来写 current >= 阈值，只认充电方向；放电（负电流）被当成静置，
 *      安时积分根本不跑。→ 改为对电流取绝对值比较。
 *   2. 积分符号原来写成 基准 - ΔQ，与"充电为正"的约定相反，充电时 SOC 反降。
 *      → 改为 基准 + ΔQ。
 *   3. 增加 [0,100] 钳位，避免积分漂出区间。
 *   4. soc_init() 原来置 current_existing=1，导致第一段电流的基准点抓不到。
 *      → 改为 0。
 *   5. 原来只在开机后首次（last_current_time==0）用 OCV 定初值，带载上电会从
 *      0 开始积分且长期贴地。→ 改为延迟播种：等 min_voltage 有效后用 OCV 定
 *      初值，之后才进入积分/静置校准逻辑。
 * ========================================================================== */
#include "soc_estimation.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>
#include <stddef.h>

static float ocv_soc_update(uint16_t ocv);

static const ocv_soc OCV_SOC_TABLE[] = {
    {3000, 0.00f},  {3554, 10.00f},  {3721, 20.00f},
    {3762, 30.00f}, {3778, 40.00f},  {3796, 50.00f},
    {3843, 60.00f}, {3921, 70.00f},  {3981, 80.00f},
    {4085, 90.00f}, {4195, 100.00f}
};

float    current_soc;
int      current_existing;
uint32_t last_current_time;
float    before_soc;
float    before_mAs;

static int soc_initialized;            /* 是否已用 min_voltage 播种过初值 */

/* 有电流：充电为正、放电为负，故对电流取绝对值与阈值比较 */
static int soc_has_current(int32_t current) {
    return (current >= NO_CURRENT_THRESHOLD) || (current <= -NO_CURRENT_THRESHOLD);
}

void soc_init(void) {
    current_soc       = 0.0f;
    before_soc        = 0.0f;
    before_mAs        = 0.0f;
    last_current_time = 0U;
    current_existing  = 0;
    soc_initialized   = 0;
}

void soc_update(void) {
    uint32_t now         = HAL_GetTick();
    int      has_current = soc_has_current(shunt_data.current);

    /* 起点：等 min_voltage 有效后用 OCV 表定初值。
     * 否则带载上电会从 0 开始积分，读数长期贴地。 */
    if(!soc_initialized) {
        if(min_voltage == 0U || min_voltage == 0xFFFFU) {
            return;                    /* 单体电压还没到，先不出数 */
        }
        current_soc       = ocv_soc_update(min_voltage);
        before_soc        = current_soc;
        before_mAs        = shunt_data.current_counter;
        current_existing  = has_current;
        last_current_time = has_current ? now : 0U;
        soc_initialized   = 1;
        return;
    }

    if(has_current) {
        if(!current_existing) {        /* 新电流段起点，记基准 */
            before_soc = current_soc;
            before_mAs = shunt_data.current_counter;
        }
        last_current_time = now;
        current_existing  = 1;
    }
    else {
        current_existing = 0;
    }

    if(now - last_current_time >= NO_CURRENT_TIME) {
        current_soc = ocv_soc_update(min_voltage);       /* 静置够久 → OCV 重校 */
    }
    else {
        float mAs_delta = shunt_data.current_counter - before_mAs;
        float soc_delta = mAs_delta / (float)CELL_CAPACITY * 100.0f;
        current_soc = before_soc + soc_delta;            /* 充电为正 → SOC 上升 */
    }

    if(current_soc < 0.0f)   { current_soc = 0.0f; }
    if(current_soc > 100.0f) { current_soc = 100.0f; }
}

/* OCV-SOC 查表 + 线性插值。低于表首值返回 0%，高于表尾值返回 100%。 */
static float ocv_soc_update(uint16_t ocv) {
    size_t i = 0;
    size_t array_length = sizeof(OCV_SOC_TABLE) / sizeof(*OCV_SOC_TABLE);
    while(i < array_length && OCV_SOC_TABLE[i].ocv <= ocv) {
        i++;
    }

    if(i == 0) {
        return OCV_SOC_TABLE[i].soc;
    }

    if(i == array_length) {
        return OCV_SOC_TABLE[array_length - 1].soc;
    }

    uint16_t ocv1 = OCV_SOC_TABLE[i - 1].ocv;
    uint16_t ocv2 = OCV_SOC_TABLE[i].ocv;
    float soc1 = OCV_SOC_TABLE[i - 1].soc;
    float soc2 = OCV_SOC_TABLE[i].soc;

    float slope = (soc2 - soc1) / (float)(ocv2 - ocv1);
    float interpolate_soc = soc1 + slope * (float)(ocv - ocv1);
    return interpolate_soc;
}
