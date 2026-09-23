/* ==========================================================================
 * soc_estimation.c —— SOC 估算（BMS_V1.0 610b476 新版算法）
 *
 * 调用约定：soc_update() 读 shunt_data / 状态机 / 行车模式并写 current_soc，
 * 由控制线程在 bms_state_lock() 内调用（current_soc 会被遥测线程读取）。
 *
 * 符号约定（与 shunt_monitor 一致）：**充电为正、放电为负**，故积分是
 * integrated_soc += ΔQ/Q_full*100，充电时 SOC 上升。
 *
 * flash：SOC 与配置共用 bms_config 的 Sector 10 追加记录区。soc_save() 只在
 *   SOC 变化 ≥1%、到 0/100%、或"退出行车模式且断开高压"时触发，避免高频擦写。
 *   写满一个扇区后由 soc_storage_save() 整体擦除重写。该擦除在 STM32F407VG
 *   （单 bank）上会冻结整个 flash bank 秒级——所有从 flash 取指的代码（含中断）
 *   一起停，加不加 bms_state_lock 都一样。故这种整擦只在 INACTIVE 且三路 AIR
 *   全开的安全窗口里执行；否则挂起，等 soc_update() 下一个安全拍补写。
 *   普通追加写（非擦除）仍照常在任意时刻落盘。
 * ========================================================================== */
#include "soc_estimation.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "bms_config.h"
#include "can_custom.h"
#include "shunt_monitor.h"
#include "state_machine.h"

#define SOC_PRECISION_SCALE          100.0f
#define SOC_PERSIST_INTERVAL_PERCENT 1.0f
#define SOC_MILLI_PERCENT_MAX        10000U

float current_soc;
volatile uint8_t charge_finish = 0;

static float last_current_counter;
static float persisted_soc;
static float integrated_soc;
static uint8_t previous_drive_mode;
static TSState previous_ts_state;
static bool soc_save_pending;   /* 因不在安全窗口而被推迟的整擦落盘 */

static const ocv_soc E07_OCV_SOC_TABLE[] = {
    {3000, 0.00f},  {3554, 10.00f},  {3721, 20.00f},
    {3762, 30.00f}, {3778, 40.00f},  {3796, 50.00f},
    {3843, 60.00f}, {3921, 70.00f},  {3981, 80.00f},
    {4085, 90.00f}, {4195, 100.00f}
};

static const ocv_soc OCV_SOC_TABLE[] = {
    {3689, 8.59f},  {3727, 18.75f},  {3775, 28.91f},
    {3809, 39.06f}, {3852, 49.22f},  {3923, 59.38f},
    {4020, 69.53f}, {4123, 79.69f},  {4229, 89.84f},
    {4341, 100.00f}
};

static float clamp_soc(float soc)
{
    if (soc < 0.0f) {
        return 0.0f;
    }
    if (soc > 100.0f) {
        return 100.0f;
    }
    return soc;
}

// 将SOC四舍五入到小数点后两位
static float round_soc(float soc)
{
    return roundf(clamp_soc(soc) * SOC_PRECISION_SCALE) / SOC_PRECISION_SCALE;
}

// 将SOC转换为整数用于存入flash
static uint32_t soc_to_int(float soc)
{
    return (uint32_t)lroundf(round_soc(soc) * SOC_PRECISION_SCALE);
}

/* 整扇区擦除会冻结整个 flash bank（F407VG 单 bank），行车中执行会让 CAN/UART
 * 丢帧。故只在 INACTIVE 且三路 AIR 全开时才允许这种整擦。 */
static bool soc_erase_window_safe(void)
{
    return ts_state.current_state == TS_INACTIVE && ts_sm_get_air_bits() == 0U;
}

static bool soc_save(void)
{
    const uint32_t soc_milli_percent = soc_to_int(current_soc);

    if (soc_storage_would_erase() && !soc_erase_window_safe()) {
        soc_save_pending = true;    /* 延后：等进安全窗口再由 soc_update() 补写 */
        return false;
    }
    if (!soc_storage_save(soc_milli_percent)) {
        return false;
    }
    soc_save_pending = false;
    persisted_soc = (float)soc_milli_percent / SOC_PRECISION_SCALE;
    return true;
}

static float ocv_to_soc(uint16_t ocv_mV)
{
    const size_t table_size = sizeof(OCV_SOC_TABLE) / sizeof(OCV_SOC_TABLE[0]);

    if (ocv_mV <= OCV_SOC_TABLE[0].ocv) {
        return OCV_SOC_TABLE[0].soc;
    }
    if (ocv_mV >= OCV_SOC_TABLE[table_size - 1U].ocv) {
        return OCV_SOC_TABLE[table_size - 1U].soc;
    }

    for (size_t index = 1U; index < table_size; ++index) {
        const ocv_soc lower = OCV_SOC_TABLE[index - 1U];
        const ocv_soc upper = OCV_SOC_TABLE[index];
        if (ocv_mV <= upper.ocv) {
            const float fraction = (float)(ocv_mV - lower.ocv) /
                                   (float)(upper.ocv - lower.ocv);
            return lower.soc + fraction * (upper.soc - lower.soc);
        }
    }

    return OCV_SOC_TABLE[table_size - 1U].soc;
}

bool soc_update_from_ocv(uint16_t ocv_mV)
{
    if (ocv_mV == UINT16_MAX) {
        return false;
    }

    integrated_soc = round_soc(ocv_to_soc(ocv_mV));
    current_soc = integrated_soc;
    return soc_save();
}

void soc_init(void)
{
    uint32_t soc_milli_percent = 0U;
    if (soc_storage_load(&soc_milli_percent) && soc_milli_percent <= SOC_MILLI_PERCENT_MAX) {
        current_soc = (float)soc_milli_percent / SOC_PRECISION_SCALE;
    }
    else {
        current_soc = 0.0f;
    }

    current_soc = round_soc(current_soc);
    integrated_soc = current_soc;
    persisted_soc = current_soc;
    last_current_counter = shunt_data.current_counter;
    previous_drive_mode = drive_mode;
    previous_ts_state = ts_state.current_state;
}

void soc_update(void)
{
    /* 上一拍因不在安全窗口而挂起的落盘，进安全窗口后补写。 */
    if (soc_save_pending && soc_erase_window_safe()) {
        (void)soc_save();
    }

    const uint8_t current_drive_mode = drive_mode;
    const TSState current_ts_state = ts_state.current_state;
    const bool drive_mode_exit = previous_drive_mode == 1U && current_drive_mode == 0U;
    const bool active_state_exit = previous_ts_state == TS_ACTIVE && current_ts_state != TS_ACTIVE;
    const float current_delta_mas = shunt_data.current_counter - last_current_counter;

    last_current_counter = shunt_data.current_counter;
    previous_drive_mode = current_drive_mode;
    previous_ts_state = current_ts_state;

    if (charge_finish != 0U) {
        integrated_soc = 100.0f;
        current_soc = 100.0f;
        if (soc_save()) {
            charge_finish = 0U;
        }
        return;
    }

    /* 分流器正电流定义为充电，负电流定义为放电。 */
    integrated_soc = clamp_soc(integrated_soc + current_delta_mas / CELL_CAPACITY_MAS * 100.0f);
    current_soc = round_soc(integrated_soc);

    // SOC变化幅度大于1%时写入flash，驾驶模式退出且断开高压时写入flash
    if (fabsf(current_soc - persisted_soc) >= SOC_PERSIST_INTERVAL_PERCENT ||
        ((current_soc == 0.0f || current_soc == 100.0f) && current_soc != persisted_soc) ||
        (drive_mode_exit && active_state_exit)) {
        (void)soc_save();
    }
}
