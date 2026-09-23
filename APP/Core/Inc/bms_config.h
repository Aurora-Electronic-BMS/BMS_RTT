#ifndef INC_BMS_CONFIG_H
#define INC_BMS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The LTC6804 chain buffers are deliberately compiled for the largest supported
 * pack.  total_ic is the active chain length selected at runtime.
 */
#define BMS_MAX_IC 12U

/* STM32F407 Sector 10: 0x080C0000..0x080DFFFF (128 KiB). */
#define BMS_CONFIG_FLASH_ADDRESS 0x080C0000UL
#define BMS_CONFIG_FLASH_SIZE    0x00020000UL

#define BMS_CONFIG_MAGIC         0x43464732UL /* "CFG2" */
#define BMS_CONFIG_VERSION       3UL
#define BMS_CONFIG_COMMIT        0xA55A3CC3UL

/*
 * Every field has a fixed 32-bit representation.  This keeps the UART
 * payload independent of compiler structure padding and permits signed
 * temperature limits.
 */
typedef struct {
    int32_t total_ic;

    int32_t discharge_duration_ms;
    int32_t no_error_time_ms;
    int32_t air_close_wait_ms;
    int32_t precharge_95_wait_time_ms;
    int32_t pre_hold_time_ms;
    int32_t max_state_hold_time_ms;
    int32_t drive_mode_timeout_ms;
    int32_t max_charge_volt_dV;
    int32_t max_charge_curr_dA;

    int32_t shunt_timeout_ms;
    int32_t shunt_overcurrent_discharge_mA;
    int32_t shunt_overcurrent_charge_mA;
    int32_t shunt_overcurrent_regen_mA;
    int32_t shunt_overtemp_dC;

    int32_t max_cell_volt_mV;
    int32_t min_cell_volt_mV;
    int32_t max_cell_temp_C;
    int32_t min_cell_temp_C;
    int32_t pec_current_threshold_mA;

    int32_t shunt_timeout_confirm_ms;
    int32_t shunt_overcurrent_confirm_ms;
    int32_t shunt_overtemp_confirm_ms;
    int32_t shunt_voltage_mismatch_confirm_ms;
    int32_t slave_overvolt_confirm_ms;
    int32_t slave_undervolt_confirm_ms;
    int32_t slave_overtemp_confirm_ms;
    int32_t slave_comm_confirm_ms;

    int32_t can1_baudrate;
    int32_t can2_baudrate;
} BmsConfig;

#define BMS_CONFIG_FIELD_COUNT (sizeof(BmsConfig) / sizeof(int32_t))

extern BmsConfig bms_config;

/* Load the newest CRC-protected record, or compile-time defaults on first use. */
void bms_config_init(void);

const BmsConfig *bms_config_get(void);
bool bms_config_is_valid(const BmsConfig *candidate);

/* Applies a validated configuration in RAM and updates all monitor delays. */
bool bms_config_apply(const BmsConfig *candidate);

/* Appends a new record to Sector 10.  A successful return means it was read-back verified. */
bool bms_config_save(const BmsConfig *candidate);

/* SOC 与配置共享 Sector 10 的追加记录区，数值单位为 0.001%。 */
bool soc_storage_load(uint32_t *soc_milli_percent);
bool soc_storage_save(uint32_t soc_milli_percent);

/* 下一个 SOC 记录是否会触发整扇区擦除（即追加区已满）。调用者据此把这次
 * 秒级、冻结整个 flash bank 的擦除推迟到 INACTIVE 的安全窗口再做。 */
bool soc_storage_would_erase(void);

#endif
