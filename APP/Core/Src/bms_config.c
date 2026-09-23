#include "bms_config.h"

#include <stddef.h>
#include <string.h>

#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "stm32f4xx_hal.h"

#define BMS_FLASH_SLOT_SIZE      256U
#define BMS_FLASH_SLOT_COUNT     (BMS_CONFIG_FLASH_SIZE / BMS_FLASH_SLOT_SIZE)
#define SOC_STORAGE_MAGIC        0x534F4331UL /* "SOC1" */
#define SOC_STORAGE_VERSION      1UL
#define SOC_STORAGE_COMMIT       0x5A3CC3A5UL
#define SOC_MILLI_PERCENT_MAX    100000U

#define LEGACY_CONFIG_MAGIC      0x43464731UL /* "CFG1" */
#define LEGACY_CONFIG_VERSION    1UL
#define BMS_CONFIG_VERSION_V2    2UL

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
} BmsConfigV2;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    BmsConfig config;
    uint8_t reserved[BMS_FLASH_SLOT_SIZE - sizeof(uint32_t) * 5U - sizeof(BmsConfig)];
    uint32_t crc32;
    uint32_t commit;
} BmsConfigRecord;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    BmsConfigV2 config;
    uint8_t reserved[BMS_FLASH_SLOT_SIZE - sizeof(uint32_t) * 5U - sizeof(BmsConfigV2)];
    uint32_t crc32;
    uint32_t commit;
} BmsConfigRecordV2;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t soc_milli_percent;
    uint8_t reserved[BMS_FLASH_SLOT_SIZE - sizeof(uint32_t) * 6U];
    uint32_t crc32;
    uint32_t commit;
} SocStorageRecord;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    BmsConfigV2 config;
    uint32_t crc32;
    uint32_t commit;
} LegacyBmsConfigRecord;

_Static_assert((sizeof(BmsConfig) % sizeof(uint32_t)) == 0U,
               "BmsConfig must be word aligned");
_Static_assert(BMS_CONFIG_FIELD_COUNT == 30U,
               "UART configuration payload field count changed");
_Static_assert(sizeof(BmsConfigRecord) == BMS_FLASH_SLOT_SIZE,
               "BmsConfigRecord must occupy one flash slot");
_Static_assert(sizeof(SocStorageRecord) == BMS_FLASH_SLOT_SIZE,
               "SocStorageRecord must occupy one flash slot");

static const BmsConfig default_config = {
    .total_ic = 12,
    .discharge_duration_ms = 5000,
    .no_error_time_ms = 1000,
    .air_close_wait_ms = 10,
    .precharge_95_wait_time_ms = 5000,
    .pre_hold_time_ms = 200,
    .max_state_hold_time_ms = 500,
    .drive_mode_timeout_ms = 2000,
    .max_charge_volt_dV = 5800,
    .max_charge_curr_dA = 30,
    .shunt_timeout_ms = 500,
    .shunt_overcurrent_discharge_mA = 200000,
    .shunt_overcurrent_charge_mA = 15000,
    .shunt_overcurrent_regen_mA = 25000,
    .shunt_overtemp_dC = 1000,
    .max_cell_volt_mV = 4200,
    .min_cell_volt_mV = 3000,
    .max_cell_temp_C = 50,
    .min_cell_temp_C = 0,
    .pec_current_threshold_mA = 1000,
    .shunt_timeout_confirm_ms = 500,
    .shunt_overcurrent_confirm_ms = 500,
    .shunt_overtemp_confirm_ms = 1000,
    .shunt_voltage_mismatch_confirm_ms = 1000,
    .slave_overvolt_confirm_ms = 1000,
    .slave_undervolt_confirm_ms = 1000,
    .slave_overtemp_confirm_ms = 1000,
    .slave_comm_confirm_ms = 1000,
    .can1_baudrate = 500000,
    .can2_baudrate = 500000,
};

BmsConfig bms_config;
static bool flash_layout_ready;

static uint32_t crc32_ieee(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    while (length-- > 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
        }
    }
    return ~crc;
}

static bool field_in_range(int32_t value, int32_t minimum, int32_t maximum)
{
    return value >= minimum && value <= maximum;
}

bool bms_config_is_valid(const BmsConfig *candidate)
{
    if (candidate == NULL ||
        !field_in_range(candidate->total_ic, 1, (int32_t)BMS_MAX_IC) ||
        !field_in_range(candidate->discharge_duration_ms, 0, 5000) ||
        !field_in_range(candidate->no_error_time_ms, 0, 5000) ||
        !field_in_range(candidate->air_close_wait_ms, 0, 100) ||
        !field_in_range(candidate->precharge_95_wait_time_ms, 1, 5000) ||
        !field_in_range(candidate->pre_hold_time_ms, 0, 1000) ||
        !field_in_range(candidate->max_state_hold_time_ms, 0, 60000) ||
        !field_in_range(candidate->drive_mode_timeout_ms, 1, 5000) ||
        !field_in_range(candidate->max_charge_volt_dV, 0, 6000) ||
        !field_in_range(candidate->max_charge_curr_dA, 0, 50) ||
        !field_in_range(candidate->shunt_timeout_ms, 1, 5000) ||
        !field_in_range(candidate->shunt_overcurrent_discharge_mA, 0, 300000) ||
        !field_in_range(candidate->shunt_overcurrent_charge_mA, 0, 50000) ||
        !field_in_range(candidate->shunt_overcurrent_regen_mA, 0, 50000) ||
        !field_in_range(candidate->shunt_overtemp_dC, -20, 1500) ||
        !field_in_range(candidate->min_cell_volt_mV, 1500, 4500) ||
        !field_in_range(candidate->max_cell_volt_mV, 2500, 5000) ||
        candidate->min_cell_volt_mV >= candidate->max_cell_volt_mV ||
        !field_in_range(candidate->min_cell_temp_C, -40, 100) ||
        !field_in_range(candidate->max_cell_temp_C, -40, 120) ||
        candidate->min_cell_temp_C >= candidate->max_cell_temp_C ||
        !field_in_range(candidate->pec_current_threshold_mA, 0, 5000) ||
        (candidate->can1_baudrate != 250000 && candidate->can1_baudrate != 500000) ||
        (candidate->can2_baudrate != 250000 && candidate->can2_baudrate != 500000)) {
        return false;
    }

    const int32_t *delays = &candidate->shunt_timeout_confirm_ms;
    for (uint32_t index = 0U; index < 8U; ++index) {
        if (!field_in_range(delays[index], 0, 600000)) {
            return false;
        }
    }
    return true;
}

static BmsConfig config_from_v2(const BmsConfigV2 *config)
{
    BmsConfig migrated = default_config;
    memcpy(&migrated, config, sizeof(*config));
    return migrated;
}

static bool config_record_is_valid(const BmsConfigRecord *record)
{
    if (record->magic != BMS_CONFIG_MAGIC ||
        record->version != BMS_CONFIG_VERSION ||
        record->commit != BMS_CONFIG_COMMIT ||
        !bms_config_is_valid(&record->config)) {
        return false;
    }
    return record->crc32 == crc32_ieee((const uint8_t *)record,
                                       (uint32_t)offsetof(BmsConfigRecord, crc32));
}

static bool soc_record_is_valid(const SocStorageRecord *record)
{
    if (record->magic != SOC_STORAGE_MAGIC ||
        record->version != SOC_STORAGE_VERSION ||
        record->commit != SOC_STORAGE_COMMIT ||
        record->soc_milli_percent > SOC_MILLI_PERCENT_MAX) {
        return false;
    }
    return record->crc32 == crc32_ieee((const uint8_t *)record,
                                       (uint32_t)offsetof(SocStorageRecord, crc32));
}

static bool config_record_v2_is_valid(const BmsConfigRecordV2 *record)
{
    if (record->magic != BMS_CONFIG_MAGIC ||
        record->version != BMS_CONFIG_VERSION_V2 ||
        record->commit != BMS_CONFIG_COMMIT) {
        return false;
    }
    return record->crc32 == crc32_ieee((const uint8_t *)record,
                                       (uint32_t)offsetof(BmsConfigRecordV2, crc32));
}

static bool legacy_config_record_is_valid(const LegacyBmsConfigRecord *record)
{
    BmsConfig migrated;
    if (record == NULL) {
        return false;
    }
    migrated = config_from_v2(&record->config);
    if (record->magic != LEGACY_CONFIG_MAGIC ||
        record->version != LEGACY_CONFIG_VERSION ||
        record->commit != BMS_CONFIG_COMMIT ||
        !bms_config_is_valid(&migrated)) {
        return false;
    }
    return record->crc32 == crc32_ieee((const uint8_t *)record,
                                       (uint32_t)offsetof(LegacyBmsConfigRecord, crc32));
}

static const uint8_t *slot_at(uint32_t index)
{
    return (const uint8_t *)(BMS_CONFIG_FLASH_ADDRESS + index * BMS_FLASH_SLOT_SIZE);
}

static bool slot_is_erased(uint32_t index)
{
    const uint32_t *words = (const uint32_t *)slot_at(index);
    for (uint32_t word = 0U; word < BMS_FLASH_SLOT_SIZE / sizeof(uint32_t); ++word) {
        if (words[word] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    return true;
}

static bool program_slot(uint32_t index, const void *record)
{
    const uint32_t *words = (const uint32_t *)record;
    const uint32_t word_count = BMS_FLASH_SLOT_SIZE / sizeof(uint32_t);
    const uint32_t address = BMS_CONFIG_FLASH_ADDRESS + index * BMS_FLASH_SLOT_SIZE;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    bool ok = true;
    /* 提交字在最后写入，掉电中断的记录不会在启动时生效。 */
    for (uint32_t word = 0U; word < word_count - 1U; ++word) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              address + word * sizeof(uint32_t), words[word]) != HAL_OK) {
            ok = false;
            break;
        }
    }
    if (ok && HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                address + (word_count - 1U) * sizeof(uint32_t),
                                words[word_count - 1U]) != HAL_OK) {
        ok = false;
    }
    (void)HAL_FLASH_Lock();
    return ok;
}

static bool erase_config_sector(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0U;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_10;
    erase.NbSectors = 1U;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    return status == HAL_OK && sector_error == 0xFFFFFFFFUL;
}

static void scan_records(const BmsConfigRecord **newest_config,
                         const SocStorageRecord **newest_soc,
                         uint32_t *free_slot)
{
    *newest_config = NULL;
    *newest_soc = NULL;
    *free_slot = BMS_FLASH_SLOT_COUNT;

    for (uint32_t index = 0U; index < BMS_FLASH_SLOT_COUNT; ++index) {
        const BmsConfigRecord *config_record = (const BmsConfigRecord *)slot_at(index);
        const SocStorageRecord *soc_record = (const SocStorageRecord *)slot_at(index);

        if (config_record_is_valid(config_record) &&
            (*newest_config == NULL || config_record->generation > (*newest_config)->generation)) {
            *newest_config = config_record;
        }
        if (soc_record_is_valid(soc_record) &&
            (*newest_soc == NULL || soc_record->generation > (*newest_soc)->generation)) {
            *newest_soc = soc_record;
        }
        if (*free_slot == BMS_FLASH_SLOT_COUNT && slot_is_erased(index)) {
            *free_slot = index;
        }
    }
}

static const LegacyBmsConfigRecord *find_legacy_config(void)
{
    const LegacyBmsConfigRecord *newest = NULL;
    const uint32_t count = BMS_CONFIG_FLASH_SIZE / sizeof(LegacyBmsConfigRecord);
    for (uint32_t index = 0U; index < count; ++index) {
        const LegacyBmsConfigRecord *record = (const LegacyBmsConfigRecord *)(
            BMS_CONFIG_FLASH_ADDRESS + index * sizeof(LegacyBmsConfigRecord));
        if (legacy_config_record_is_valid(record) &&
            (newest == NULL || record->generation > newest->generation)) {
            newest = record;
        }
    }
    return newest;
}

static const BmsConfigRecordV2 *find_v2_config(void)
{
    const BmsConfigRecordV2 *newest = NULL;
    for (uint32_t index = 0U; index < BMS_FLASH_SLOT_COUNT; ++index) {
        const BmsConfigRecordV2 *record = (const BmsConfigRecordV2 *)slot_at(index);
        if (config_record_v2_is_valid(record) &&
            (newest == NULL || record->generation > newest->generation)) {
            newest = record;
        }
    }
    return newest;
}

static bool write_config_record(uint32_t slot, const BmsConfig *config, uint32_t generation)
{
    BmsConfigRecord record;
    memset(&record, 0, sizeof(record));
    record.magic = BMS_CONFIG_MAGIC;
    record.version = BMS_CONFIG_VERSION;
    record.generation = generation;
    record.config = *config;
    record.commit = BMS_CONFIG_COMMIT;
    record.crc32 = crc32_ieee((const uint8_t *)&record,
                              (uint32_t)offsetof(BmsConfigRecord, crc32));

    if (!program_slot(slot, &record)) {
        return false;
    }
    const BmsConfigRecord *written = (const BmsConfigRecord *)slot_at(slot);
    return config_record_is_valid(written) && written->generation == generation;
}

static bool write_soc_record(uint32_t slot, uint32_t soc_milli_percent, uint32_t generation)
{
    SocStorageRecord record;
    memset(&record, 0, sizeof(record));
    record.magic = SOC_STORAGE_MAGIC;
    record.version = SOC_STORAGE_VERSION;
    record.generation = generation;
    record.soc_milli_percent = soc_milli_percent;
    record.commit = SOC_STORAGE_COMMIT;
    record.crc32 = crc32_ieee((const uint8_t *)&record,
                              (uint32_t)offsetof(SocStorageRecord, crc32));

    if (!program_slot(slot, &record)) {
        return false;
    }
    const SocStorageRecord *written = (const SocStorageRecord *)slot_at(slot);
    return soc_record_is_valid(written) && written->generation == generation;
}

static bool erase_and_rewrite(const BmsConfig *config, uint32_t config_generation,
                              bool has_soc, uint32_t soc_milli_percent,
                              uint32_t soc_generation)
{
    if (!erase_config_sector() || !write_config_record(0U, config, config_generation)) {
        return false;
    }
    if (has_soc && !write_soc_record(1U, soc_milli_percent, soc_generation)) {
        return false;
    }
    return true;
}

void bms_config_init(void)
{
    const BmsConfigRecord *newest_config;
    const SocStorageRecord *newest_soc;
    uint32_t free_slot;
    flash_layout_ready = false;
    scan_records(&newest_config, &newest_soc, &free_slot);
    (void)free_slot;

    if (newest_config != NULL) {
        bms_config = newest_config->config;
        flash_layout_ready = true;
    } else {
        const BmsConfigRecordV2 *v2_config = find_v2_config();
        const LegacyBmsConfigRecord *legacy_config = v2_config == NULL ? find_legacy_config() : NULL;
        bms_config = v2_config != NULL ? config_from_v2(&v2_config->config) :
                     legacy_config == NULL ? default_config : config_from_v2(&legacy_config->config);

        /* 旧版配置首次启动时迁移，并为两个 CAN 通道填充 500 kbit/s 默认值。 */
        if (v2_config != NULL || legacy_config != NULL) {
            const uint32_t generation = v2_config != NULL ? v2_config->generation : legacy_config->generation;
            flash_layout_ready = erase_and_rewrite(&bms_config, generation,
                                                    newest_soc != NULL,
                                                    newest_soc == NULL ? 0U : newest_soc->soc_milli_percent,
                                                    newest_soc == NULL ? 0U : newest_soc->generation);
        } else {
            flash_layout_ready = true;
        }
    }
    shunt_apply_config();
    slaves_apply_config();
}

const BmsConfig *bms_config_get(void)
{
    return &bms_config;
}

bool bms_config_apply(const BmsConfig *candidate)
{
    if (!bms_config_is_valid(candidate)) {
        return false;
    }
    bms_config = *candidate;
    shunt_apply_config();
    slaves_apply_config();
    return true;
}

bool bms_config_save(const BmsConfig *candidate)
{
    if (!flash_layout_ready || !bms_config_is_valid(candidate)) {
        return false;
    }

    const BmsConfigRecord *newest_config;
    const SocStorageRecord *newest_soc;
    uint32_t free_slot;
    scan_records(&newest_config, &newest_soc, &free_slot);

    const uint32_t config_generation = newest_config == NULL ? 1U : newest_config->generation + 1U;
    bool ok;
    if (free_slot == BMS_FLASH_SLOT_COUNT) {
        ok = erase_and_rewrite(candidate, config_generation,
                               newest_soc != NULL, newest_soc == NULL ? 0U : newest_soc->soc_milli_percent,
                               newest_soc == NULL ? 0U : newest_soc->generation);
    } else {
        ok = write_config_record(free_slot, candidate, config_generation);
    }
    return ok && bms_config_apply(candidate);
}

bool soc_storage_load(uint32_t *soc_milli_percent)
{
    if (!flash_layout_ready || soc_milli_percent == NULL) {
        return false;
    }

    const BmsConfigRecord *newest_config;
    const SocStorageRecord *newest_soc;
    uint32_t free_slot;
    scan_records(&newest_config, &newest_soc, &free_slot);
    (void)newest_config;
    (void)free_slot;

    if (newest_soc == NULL) {
        return false;
    }
    *soc_milli_percent = newest_soc->soc_milli_percent;
    return true;
}

bool soc_storage_save(uint32_t soc_milli_percent)
{
    if (!flash_layout_ready || soc_milli_percent > SOC_MILLI_PERCENT_MAX) {
        return false;
    }

    const BmsConfigRecord *newest_config;
    const SocStorageRecord *newest_soc;
    uint32_t free_slot;
    scan_records(&newest_config, &newest_soc, &free_slot);

    const uint32_t soc_generation = newest_soc == NULL ? 1U : newest_soc->generation + 1U;
    if (free_slot != BMS_FLASH_SLOT_COUNT) {
        return write_soc_record(free_slot, soc_milli_percent, soc_generation);
    }

    const BmsConfig *config = newest_config == NULL ? &bms_config : &newest_config->config;
    const uint32_t config_generation = newest_config == NULL ? 1U : newest_config->generation + 1U;
    return erase_and_rewrite(config, config_generation, true, soc_milli_percent, soc_generation);
}

bool soc_storage_would_erase(void)
{
    const BmsConfigRecord *newest_config;
    const SocStorageRecord *newest_soc;
    uint32_t free_slot;

    scan_records(&newest_config, &newest_soc, &free_slot);
    (void)newest_config;
    (void)newest_soc;
    return free_slot == BMS_FLASH_SLOT_COUNT;
}
