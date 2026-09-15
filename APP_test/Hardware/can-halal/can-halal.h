#ifndef CAN_HALAL_H
#define CAN_HALAL_H

// Define family macros if none are defined and we recognize a chip macro
#if !defined(STM32F3) && !defined(STM32H7) && !defined(STM32F0) && !defined(STM32F4)
#if defined(STM32F302x6) || defined(STM32F302x8) || defined(STM32F302xB) ||    \
    defined(STM32F302xC)
#define STM32F3
#endif
#if defined(STM32H7A3xx) || defined(STM32H723xx)
#define STM32H7
#endif
#if defined(STM32F042x6)
#define STM32F0
#endif
#if defined(STM32F407xx)
#define STM32F4
#endif
#endif

#if defined(STM32F3)
    #include "stm32f3xx_hal.h"
    #define FTCAN_IS_BXCAN
    #define FTCAN_NUM_FILTERS 13
#elif defined(STM32H7)
    #include "stm32h7xx_hal.h"
    #define FTCAN_IS_FDCAN
    #ifndef FTCAN_NUM_FILTERS
        #error "Please configure the number of filters in CubeMX, and then add a compiler define for FTCAN_NUM_FILTERS"
    #endif
#elif defined(STM32F0)
    #include "stm32f0xx_hal.h"
    #define FTCAN_IS_BXCAN
    #define FTCAN_NUM_FILTERS 13
#elif defined(STM32F4)
    #include "stm32f4xx_hal.h"
    #define FTCAN_IS_BXCAN
    #define FTCAN_NUM_FILTERS 14
    #define FTCAN_BXCAN_CONTEXT_COUNT 2
    #define FTCAN_FILTER_BANK_COUNT 28
    #define FTCAN_CAN2_START_FILTER_BANK 14
#else
    #error "Couldn't detect STM family"
#endif

#if defined(FTCAN_IS_BXCAN)
/*
 * CAN1 and CAN2 on STM32F407 share 28 filter banks.  CAN1 owns the banks
 * below FTCAN_CAN2_START_FILTER_BANK and CAN2 owns the remaining banks.
 */
#ifndef FTCAN_BXCAN_CONTEXT_COUNT
#define FTCAN_BXCAN_CONTEXT_COUNT 1
#endif
#ifndef FTCAN_FILTER_BANK_COUNT
#define FTCAN_FILTER_BANK_COUNT FTCAN_NUM_FILTERS
#endif
#ifndef FTCAN_CAN2_START_FILTER_BANK
#define FTCAN_CAN2_START_FILTER_BANK FTCAN_NUM_FILTERS
#endif

typedef enum {
    FTCAN_BUS_1 = 0,
    FTCAN_BUS_2 = 1,
} ftcan_bus_t;

typedef struct {
    CAN_HandleTypeDef *handle;
    ftcan_bus_t bus;
    uint8_t first_filter_bank;
    uint8_t next_filter_bank;
    uint8_t end_filter_bank;
} ftcan_t;

HAL_StatusTypeDef ftcan_context_init(ftcan_t *ctx,
                                     CAN_HandleTypeDef *handle,
                                     ftcan_bus_t bus,
                                     uint8_t first_filter_bank,
                                     uint8_t end_filter_bank);
HAL_StatusTypeDef ftcan_start(ftcan_t *ctx);

HAL_StatusTypeDef ftcan_transmit(ftcan_t *ctx, uint32_t id,
                                 const uint8_t *data, size_t datalen);
HAL_StatusTypeDef ftcan_transmit_ext(ftcan_t *ctx, uint32_t id,
                                     const uint8_t *data, size_t datalen);

HAL_StatusTypeDef ftcan_add_filter(ftcan_t *ctx, uint16_t id, uint16_t mask);
HAL_StatusTypeDef ftcan_add_ext_filter(ftcan_t *ctx, uint32_t id,
                                       uint32_t mask);

void ftcan_msg_received_cb(ftcan_t *ctx, uint32_t id, size_t datalen,
                           const uint8_t *data);
#elif defined(FTCAN_IS_FDCAN)
HAL_StatusTypeDef ftcan_init(FDCAN_HandleTypeDef *handle);
HAL_StatusTypeDef ftcan_transmit(uint32_t id, const uint8_t *data,
                                 size_t datalen);
HAL_StatusTypeDef ftcan_transmit_ext(uint32_t id, const uint8_t *data,
                                     size_t datalen);

HAL_StatusTypeDef ftcan_add_filter(uint16_t id, uint16_t mask);
HAL_StatusTypeDef ftcan_add_ext_filter(uint32_t id, uint32_t mask);

void ftcan_msg_received_cb(uint32_t id, size_t datalen, const uint8_t *data);
#else
#error "Unknown CAN peripheral"
#endif

/**
 * Read num_bytes bytes from a message (unmarshalled network byte order). The
 * msg pointer is advanced by the corresponding number of bytes.
 *
 * Both methods return a 64-bit integer, but you can safely cast it to a smaller
 * integer type.
 */
uint64_t ftcan_unmarshal_unsigned(const uint8_t **data, size_t num_bytes);
int64_t ftcan_unmarshal_signed(const uint8_t **data, size_t num_bytes);

/**
 * Write num_bytes to a message (marshalled in network byte order). The pointer
 * is advanced by the corresponding number of bytes and returned.
 */
uint8_t *ftcan_marshal_unsigned(uint8_t *data, uint64_t val, size_t num_bytes);
uint8_t *ftcan_marshal_signed(uint8_t *data, int64_t val, size_t num_bytes);

#endif // CAN_HALAL_H
