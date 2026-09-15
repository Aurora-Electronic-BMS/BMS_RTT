#include "can-halal.h"

#include <string.h>

#if defined(FTCAN_IS_BXCAN)
static ftcan_t *can_contexts[FTCAN_BXCAN_CONTEXT_COUNT];

static HAL_StatusTypeDef ftcan_config_filter(ftcan_t *ctx, uint32_t id,
                                             uint32_t mask,
                                             uint32_t ide) {
  uint32_t filter_id;
  uint32_t filter_mask;

  if (ctx == NULL || ctx->handle == NULL ||
      ctx->next_filter_bank >= ctx->end_filter_bank) {
    return HAL_ERROR;
  }

  if (ide == CAN_ID_EXT) {
    filter_id = ((id & 0x1FFFFFFFU) << 3) | CAN_ID_EXT;
    filter_mask = ((mask & 0x1FFFFFFFU) << 3) | CAN_ID_EXT;
  } else {
    filter_id = (id & 0x7FFU) << 21;
    filter_mask = ((mask & 0x7FFU) << 21) | CAN_ID_EXT;
  }

  CAN_FilterTypeDef filter = {0};
  filter.FilterIdHigh = filter_id >> 16;
  filter.FilterIdLow = filter_id & 0xFFFFU;
  filter.FilterMaskIdHigh = filter_mask >> 16;
  filter.FilterMaskIdLow = filter_mask & 0xFFFFU;
  filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  filter.FilterBank = ctx->next_filter_bank;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterActivation = CAN_FILTER_ENABLE;
  filter.SlaveStartFilterBank = FTCAN_CAN2_START_FILTER_BANK;

  HAL_StatusTypeDef status = HAL_CAN_ConfigFilter(ctx->handle, &filter);
  if (status == HAL_OK) {
    ctx->next_filter_bank++;
  }
  return status;
}

HAL_StatusTypeDef ftcan_context_init(ftcan_t *ctx,
                                     CAN_HandleTypeDef *handle,
                                     ftcan_bus_t bus,
                                     uint8_t first_filter_bank,
                                     uint8_t end_filter_bank) {
  if (ctx == NULL || handle == NULL || bus >= FTCAN_BXCAN_CONTEXT_COUNT ||
      first_filter_bank >= end_filter_bank ||
      end_filter_bank > FTCAN_FILTER_BANK_COUNT) {
    return HAL_ERROR;
  }

  ctx->handle = handle;
  ctx->bus = bus;
  ctx->first_filter_bank = first_filter_bank;
  ctx->next_filter_bank = first_filter_bank;
  ctx->end_filter_bank = end_filter_bank;
  can_contexts[bus] = ctx;

  return HAL_OK;
}

HAL_StatusTypeDef ftcan_start(ftcan_t *ctx) {
  if (ctx == NULL || ctx->handle == NULL) {
    return HAL_ERROR;
  }

  HAL_StatusTypeDef status =
      HAL_CAN_ActivateNotification(ctx->handle, CAN_IT_RX_FIFO0_MSG_PENDING);
  if (status != HAL_OK) {
    return status;
  }

  return HAL_CAN_Start(ctx->handle);
}

static HAL_StatusTypeDef ftcan_transmit_with_id_type(ftcan_t *ctx,
                                                     uint32_t id,
                                                     uint32_t ide,
                                                     const uint8_t *data,
                                                     size_t datalen) {
  if (ctx == NULL || ctx->handle == NULL || data == NULL || datalen > 8U) {
    return HAL_ERROR;
  }

  CAN_TxHeaderTypeDef header = {0};
  if (ide == CAN_ID_EXT) {
    header.ExtId = id & 0x1FFFFFFFU;
  } else {
    header.StdId = id & 0x7FFU;
  }
  header.IDE = ide;
  header.RTR = CAN_RTR_DATA;
  header.DLC = datalen;
  uint32_t mailbox;
  return HAL_CAN_AddTxMessage(ctx->handle, &header, (uint8_t *)data, &mailbox);
}

HAL_StatusTypeDef ftcan_transmit(ftcan_t *ctx, uint32_t id,
                                 const uint8_t *data,
                                 size_t datalen) {
  uint32_t ide = id > 0x7FFU ? CAN_ID_EXT : CAN_ID_STD;
  return ftcan_transmit_with_id_type(ctx, id, ide, data, datalen);
}

HAL_StatusTypeDef ftcan_transmit_ext(ftcan_t *ctx, uint32_t id,
                                     const uint8_t *data,
                                     size_t datalen) {
  return ftcan_transmit_with_id_type(ctx, id, CAN_ID_EXT, data, datalen);
}

HAL_StatusTypeDef ftcan_add_filter(ftcan_t *ctx, uint16_t id, uint16_t mask) {
  return ftcan_config_filter(ctx, id, mask, CAN_ID_STD);
}

HAL_StatusTypeDef ftcan_add_ext_filter(ftcan_t *ctx, uint32_t id,
                                       uint32_t mask) {
  return ftcan_config_filter(ctx, id, mask, CAN_ID_EXT);
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle) {
  ftcan_t *ctx = NULL;
  for (size_t i = 0; i < FTCAN_BXCAN_CONTEXT_COUNT; i++) {
    if (can_contexts[i] != NULL && can_contexts[i]->handle == handle) {
      ctx = can_contexts[i];
      break;
    }
  }

  if (ctx == NULL) {
    return;
  }

  CAN_RxHeaderTypeDef header;
  uint8_t data[8];
  if (HAL_CAN_GetRxMessage(handle, CAN_RX_FIFO0, &header, data) != HAL_OK) {
    return;
  }

  if (header.IDE == CAN_ID_STD) {
    ftcan_msg_received_cb(ctx, header.StdId, header.DLC, data);
  } else if (header.IDE == CAN_ID_EXT) {
    ftcan_msg_received_cb(ctx, header.ExtId, header.DLC, data);
  }
}
#elif defined(FTCAN_IS_FDCAN)
static FDCAN_HandleTypeDef *hcan1;

HAL_StatusTypeDef ftcan_init(FDCAN_HandleTypeDef *handle) {
  hcan1 = handle;

  HAL_StatusTypeDef status =
      HAL_FDCAN_ActivateNotification(hcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
  if (status != HAL_OK) {
    return status;
  }
  // Reject non-matching messages
  status =
      HAL_FDCAN_ConfigGlobalFilter(hcan1, FDCAN_REJECT, FDCAN_REJECT,
                                   FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);
  if (status != HAL_OK) {
    return status;
  }

  return HAL_FDCAN_Start(hcan1);
}

static HAL_StatusTypeDef ftcan_transmit_with_id_type(uint32_t id,
                                                     uint32_t id_type,
                                                     const uint8_t *data,
                                                     size_t datalen) {
  static FDCAN_TxHeaderTypeDef header;
  header.Identifier = id;
  header.IdType = id_type;
  header.TxFrameType = FDCAN_DATA_FRAME;
  switch (datalen) {
  case 0:
    header.DataLength = FDCAN_DLC_BYTES_0;
    break;
  case 1:
    header.DataLength = FDCAN_DLC_BYTES_1;
    break;
  case 2:
    header.DataLength = FDCAN_DLC_BYTES_2;
    break;
  case 3:
    header.DataLength = FDCAN_DLC_BYTES_3;
    break;
  case 4:
    header.DataLength = FDCAN_DLC_BYTES_4;
    break;
  case 5:
    header.DataLength = FDCAN_DLC_BYTES_5;
    break;
  case 6:
    header.DataLength = FDCAN_DLC_BYTES_6;
    break;
  case 7:
    header.DataLength = FDCAN_DLC_BYTES_7;
    break;
  case 8:
  default:
    header.DataLength = FDCAN_DLC_BYTES_8;
    break;
  }
  header.ErrorStateIndicator = FDCAN_ESI_PASSIVE;
  header.BitRateSwitch = FDCAN_BRS_OFF;
  header.FDFormat = FDCAN_CLASSIC_CAN;
  header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;

  // HAL_FDCAN_AddMessageToTxFifoQ doesn't modify the data, but it's not marked
  // as const for some reason.
  uint8_t *data_nonconst = (uint8_t *)data;
  return HAL_FDCAN_AddMessageToTxFifoQ(hcan1, &header, data_nonconst);
}

HAL_StatusTypeDef ftcan_transmit(uint32_t id, const uint8_t *data,
                                 size_t datalen) {
  uint32_t id_type = id > 0x7FFU ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
  return ftcan_transmit_with_id_type(id, id_type, data, datalen);
}

HAL_StatusTypeDef ftcan_transmit_ext(uint32_t id, const uint8_t *data,
                                     size_t datalen) {
  return ftcan_transmit_with_id_type(id, FDCAN_EXTENDED_ID, data, datalen);
}

HAL_StatusTypeDef ftcan_add_filter(uint16_t id, uint16_t mask) {
  static uint32_t next_filter_no = 0;
  static FDCAN_FilterTypeDef filter;
  filter.IdType = FDCAN_STANDARD_ID;
  filter.FilterIndex = next_filter_no;
  if (filter.FilterIndex > FTCAN_NUM_FILTERS + 1) {
    return HAL_ERROR;
  }
  filter.FilterType = FDCAN_FILTER_MASK;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = id;
  filter.FilterID2 = mask;

  HAL_StatusTypeDef status = HAL_FDCAN_ConfigFilter(hcan1, &filter);
  if (status == HAL_OK) {
    next_filter_no++;
  }
  return status;
}

HAL_StatusTypeDef ftcan_add_ext_filter(uint32_t id, uint32_t mask) {
  static uint32_t next_filter_no = 0;
  static FDCAN_FilterTypeDef filter;
  filter.IdType = FDCAN_EXTENDED_ID;
  filter.FilterIndex = next_filter_no;
  if (filter.FilterIndex > FTCAN_NUM_FILTERS + 1) {
    return HAL_ERROR;
  }
  filter.FilterType = FDCAN_FILTER_MASK;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = id;
  filter.FilterID2 = mask;

  HAL_StatusTypeDef status = HAL_FDCAN_ConfigFilter(hcan1, &filter);
  if (status == HAL_OK) {
    next_filter_no++;
  }
  return status;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *handle,
                               uint32_t RxFifo0ITs) {
  if (handle != hcan1 || (RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == RESET) {
    return;
  }

  static FDCAN_RxHeaderTypeDef header;
  static uint8_t data[8];
  if (HAL_FDCAN_GetRxMessage(hcan1, FDCAN_RX_FIFO0, &header, data) != HAL_OK) {
    return;
  }

  if (header.FDFormat != FDCAN_CLASSIC_CAN ||
      header.RxFrameType != FDCAN_DATA_FRAME ||
      (header.IdType != FDCAN_STANDARD_ID &&
       header.IdType != FDCAN_EXTENDED_ID)) {
    return;
  }

  size_t datalen;
  switch (header.DataLength) {
  case FDCAN_DLC_BYTES_0:
    datalen = 0;
    break;
  case FDCAN_DLC_BYTES_1:
    datalen = 1;
    break;
  case FDCAN_DLC_BYTES_2:
    datalen = 2;
    break;
  case FDCAN_DLC_BYTES_3:
    datalen = 3;
    break;
  case FDCAN_DLC_BYTES_4:
    datalen = 4;
    break;
  case FDCAN_DLC_BYTES_5:
    datalen = 5;
    break;
  case FDCAN_DLC_BYTES_6:
    datalen = 6;
    break;
  case FDCAN_DLC_BYTES_7:
    datalen = 7;
    break;
  case FDCAN_DLC_BYTES_8:
    datalen = 8;
    break;
  default:
    return;
  }

  ftcan_msg_received_cb(header.Identifier, datalen, data);
}
#endif

#if defined(FTCAN_IS_BXCAN)
__weak void ftcan_msg_received_cb(ftcan_t *ctx, uint32_t id, size_t datalen,
                                  const uint8_t *data) {
  (void)ctx;
  (void)id;
  (void)datalen;
  (void)data;
}
#else
__weak void ftcan_msg_received_cb(uint32_t id, size_t datalen,
                                  const uint8_t *data) {
  (void)id;
  (void)datalen;
  (void)data;
}
#endif

uint64_t ftcan_unmarshal_unsigned(const uint8_t **data_ptr, size_t num_bytes) {
  if (num_bytes > 8) {
    num_bytes = 8;
  }

  const uint8_t *data = *data_ptr;
  uint64_t result = 0;
  for (size_t i = 0; i < num_bytes; i++) {
    result <<= 8;
    result |= data[i];
  }
  *data_ptr += num_bytes;
  return result;
}

int64_t ftcan_unmarshal_signed(const uint8_t **data_ptr, size_t num_bytes) {
  if (num_bytes > 8) {
    num_bytes = 8;
  }

  uint64_t result_unsigned = ftcan_unmarshal_unsigned(data_ptr, num_bytes);
  // Sign extend by shifting left, then copying to a signed int and shifting
  // back to the right
  size_t diff_to_64 = 64 - num_bytes * 8;
  result_unsigned <<= diff_to_64;
  int64_t result;
  memcpy(&result, &result_unsigned, 8);
  return result >> diff_to_64;
}

uint8_t *ftcan_marshal_unsigned(uint8_t *data, uint64_t val, size_t num_bytes) {
  if (num_bytes > 8) {
    num_bytes = 8;
  }

  for (int i = num_bytes - 1; i >= 0; i--) {
    data[i] = val & 0xFF;
    val >>= 8;
  }

  return data + num_bytes;
}

uint8_t *ftcan_marshal_signed(uint8_t *data, int64_t val, size_t num_bytes) {
  return ftcan_marshal_unsigned(data, val, num_bytes);
}
