/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct
{
  uint8_t command;
  uint16_t sequence;
  uint16_t length;
  uint8_t payload[260];
} boot_frame_t;

typedef enum
{
  FRAME_OK = 0,
  FRAME_TIMEOUT,
  FRAME_BAD_LENGTH,
  FRAME_BAD_CRC,
  FRAME_INCOMPLETE
} frame_result_t;

typedef struct
{
  uint8_t buffer[2U + 6U + 260U + 2U];
  uint16_t index;
  uint16_t expected_length;
} boot_stream_parser_t;

typedef struct
{
  uint32_t magic;
  uint32_t magic_inverse;
  uint32_t image_length;
  uint32_t image_crc32;
  uint32_t crc_inverse;
} app_metadata_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_START_ADDRESS       0x08010000UL
#define APP_CONFIG_ADDRESS      0x080C0000UL
#define APP_METADATA_ADDRESS    0x080FFF00UL
#define APP_MAX_IMAGE_SIZE      (APP_CONFIG_ADDRESS - APP_START_ADDRESS)
#define APP_METADATA_MAGIC      0xB00710ADUL
#define BOOT_REQUEST_MAGIC      0xB007B007UL

#define MAIN_SRAM_START         0x20000000UL
#define MAIN_SRAM_END           0x20020000UL
#define CCM_SRAM_START          0x10000000UL
#define CCM_SRAM_END            0x10010000UL

#define BOOT_MAGIC_0            0xB5U
#define BOOT_MAGIC_1            0x4DU
#define BOOT_PROTOCOL_VERSION   1U
#define BOOT_MAX_PAYLOAD        260U
#define BOOT_WAIT_MS            500U
#define BOOT_FRAME_TIMEOUT_MS   1000U
#define BOOT_SESSION_IDLE_MS    5000U
#define BOOT_IMAGE_READY_MS     6000U

#define BOOT_CMD_HELLO          0x30U
#define BOOT_CMD_BEGIN          0x31U
#define BOOT_CMD_DATA           0x32U
#define BOOT_CMD_END            0x33U
#define BOOT_CMD_RUN            0x34U
#define BOOT_MSG_ACK            0x02U
#define BOOT_MSG_NACK           0x03U

#define BOOT_RESULT_OK          0U
#define BOOT_RESULT_BAD_CRC     1U
#define BOOT_RESULT_BAD_LENGTH  2U
#define BOOT_RESULT_BAD_COMMAND 3U
#define BOOT_RESULT_BAD_STATE   4U
#define BOOT_RESULT_BAD_SEQ     5U
#define BOOT_RESULT_FLASH       6U
#define BOOT_RESULT_IMAGE_CRC   7U
#define BOOT_RESULT_VECTOR      8U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
static boot_frame_t rx_frame;
static boot_stream_parser_t detection_parsers[2];
static UART_HandleTypeDef *boot_uart;
/* IAP 收帧同时轮询这两个口：USART1(PA9/PA10) 与 USART3(PB10/PB11)，与 APP 侧
 * 的主机链路一致（APP 用 telemetry_serial_init(&huart1, &huart3)）。
 * 首帧从哪个口进来，本次升级会话就固定用哪个口回 ACK / 收数据 / 收 RUN。 */
static UART_HandleTypeDef *const boot_ports[] = {&huart1, &huart3};
#define BOOT_PORT_COUNT (sizeof(boot_ports) / sizeof(boot_ports[0]))

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CRC_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static uint16_t get_u16_le(const uint8_t *data);
static uint32_t get_u32_le(const uint8_t *data);
static void put_u16_le(uint8_t *data, uint16_t value);
static uint16_t crc16_modbus(const uint8_t *data, uint32_t length);
static uint32_t crc32_ieee(const uint8_t *data, uint32_t length);
static frame_result_t receive_frame(UART_HandleTypeDef *uart, boot_frame_t *frame,
                                    uint32_t first_byte_timeout);
static frame_result_t parser_feed(boot_stream_parser_t *parser, uint8_t byte,
                                  boot_frame_t *frame);
static frame_result_t receive_frame_any(boot_frame_t *frame, uint32_t timeout);
static void send_response(uint8_t command, uint16_t sequence, uint8_t result,
                          uint16_t expected_sequence);
static bool consume_boot_request(void);
static bool app_vector_is_valid(uint32_t image_length);
static bool app_image_is_valid(void);
static bool erase_app_sectors(void);
static bool program_flash_bytes(uint32_t address, const uint8_t *data, uint32_t length);
static bool write_app_metadata(uint32_t image_length, uint32_t image_crc32);
static void run_upgrade_session(uint16_t expected_sequence, uint16_t hello_sequence);
static void jump_to_app(void) __attribute__((noreturn));

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint16_t get_u16_le(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put_u16_le(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
}

static uint16_t crc16_modbus(const uint8_t *data, uint32_t length)
{
  uint16_t crc = 0xFFFFU;

  while (length-- > 0U)
  {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8U; ++bit)
    {
      crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

/* Standard reflected CRC-32: polynomial 0xEDB88320, init/final XOR 0xFFFFFFFF. */
static uint32_t crc32_ieee(const uint8_t *data, uint32_t length)
{
  static const uint32_t nibble_table[16] = {
      0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
      0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
      0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
      0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
  };
  uint32_t crc = 0xFFFFFFFFUL;

  while (length-- > 0U)
  {
    crc ^= *data++;
    crc = (crc >> 4) ^ nibble_table[crc & 0x0FUL];
    crc = (crc >> 4) ^ nibble_table[crc & 0x0FUL];
  }
  return crc ^ 0xFFFFFFFFUL;
}

static frame_result_t receive_frame(UART_HandleTypeDef *uart, boot_frame_t *frame,
                                    uint32_t first_byte_timeout)
{
  uint8_t byte = 0U;
  uint8_t header[6];
  uint8_t crc_bytes[2];
  uint8_t crc_input[6U + BOOT_MAX_PAYLOAD];
  uint32_t start = HAL_GetTick();
  bool have_first_magic = false;

  if (__HAL_UART_GET_FLAG(uart, UART_FLAG_ORE) != RESET)
  {
    __HAL_UART_CLEAR_OREFLAG(uart);
  }

  for (;;)
  {
    uint32_t timeout = first_byte_timeout;
    if (first_byte_timeout != HAL_MAX_DELAY)
    {
      uint32_t elapsed = HAL_GetTick() - start;
      if (elapsed >= first_byte_timeout)
      {
        return FRAME_TIMEOUT;
      }
      timeout = first_byte_timeout - elapsed;
    }

    if (HAL_UART_Receive(uart, &byte, 1U, timeout) != HAL_OK)
    {
      return FRAME_TIMEOUT;
    }

    if (!have_first_magic)
    {
      have_first_magic = (byte == BOOT_MAGIC_0);
      continue;
    }
    if (byte == BOOT_MAGIC_1)
    {
      break;
    }
    have_first_magic = (byte == BOOT_MAGIC_0);
  }

  if (HAL_UART_Receive(uart, header, sizeof(header), BOOT_FRAME_TIMEOUT_MS) != HAL_OK)
  {
    return FRAME_TIMEOUT;
  }

  frame->command = header[1];
  frame->sequence = get_u16_le(&header[2]);
  frame->length = get_u16_le(&header[4]);
  if ((header[0] != BOOT_PROTOCOL_VERSION) || (frame->length > BOOT_MAX_PAYLOAD))
  {
    return FRAME_BAD_LENGTH;
  }

  if ((frame->length > 0U) &&
      (HAL_UART_Receive(uart, frame->payload, frame->length, BOOT_FRAME_TIMEOUT_MS) != HAL_OK))
  {
    return FRAME_TIMEOUT;
  }
  if (HAL_UART_Receive(uart, crc_bytes, sizeof(crc_bytes), BOOT_FRAME_TIMEOUT_MS) != HAL_OK)
  {
    return FRAME_TIMEOUT;
  }

  memcpy(crc_input, header, sizeof(header));
  memcpy(&crc_input[sizeof(header)], frame->payload, frame->length);
  if (crc16_modbus(crc_input, sizeof(header) + frame->length) != get_u16_le(crc_bytes))
  {
    return FRAME_BAD_CRC;
  }
  return FRAME_OK;
}

static frame_result_t parser_feed(boot_stream_parser_t *parser, uint8_t byte,
                                  boot_frame_t *frame)
{
  if (parser->index == 0U)
  {
    if (byte == BOOT_MAGIC_0)
    {
      parser->buffer[parser->index++] = byte;
    }
    return FRAME_INCOMPLETE;
  }

  if (parser->index == 1U)
  {
    if (byte == BOOT_MAGIC_1)
    {
      parser->buffer[parser->index++] = byte;
    }
    else
    {
      parser->index = (byte == BOOT_MAGIC_0) ? 1U : 0U;
      parser->buffer[0] = BOOT_MAGIC_0;
    }
    return FRAME_INCOMPLETE;
  }

  if (parser->index >= sizeof(parser->buffer))
  {
    parser->index = 0U;
    parser->expected_length = 0U;
    return FRAME_BAD_LENGTH;
  }
  parser->buffer[parser->index++] = byte;

  if (parser->index == 8U)
  {
    frame->command = parser->buffer[3];
    frame->sequence = get_u16_le(&parser->buffer[4]);
    frame->length = get_u16_le(&parser->buffer[6]);
    if ((parser->buffer[2] != BOOT_PROTOCOL_VERSION) ||
        (frame->length > BOOT_MAX_PAYLOAD))
    {
      parser->index = 0U;
      parser->expected_length = 0U;
      return FRAME_BAD_LENGTH;
    }
    parser->expected_length = (uint16_t)(2U + 6U + frame->length + 2U);
  }

  if ((parser->expected_length > 0U) && (parser->index >= parser->expected_length))
  {
    uint16_t received_crc = get_u16_le(&parser->buffer[8U + frame->length]);
    uint16_t computed_crc = crc16_modbus(&parser->buffer[2], 6U + frame->length);
    if (frame->length > 0U)
    {
      memcpy(frame->payload, &parser->buffer[8], frame->length);
    }
    parser->index = 0U;
    parser->expected_length = 0U;
    return (received_crc == computed_crc) ? FRAME_OK : FRAME_BAD_CRC;
  }
  return FRAME_INCOMPLETE;
}

static frame_result_t receive_frame_any(boot_frame_t *frame, uint32_t timeout)
{
  uint32_t start = HAL_GetTick();

  for (;;)
  {
    for (uint32_t port = 0U; port < BOOT_PORT_COUNT; ++port)
    {
      UART_HandleTypeDef *uart = boot_ports[port];
      if (__HAL_UART_GET_FLAG(uart, UART_FLAG_ORE) != RESET)
      {
        __HAL_UART_CLEAR_OREFLAG(uart);
      }
      if (__HAL_UART_GET_FLAG(uart, UART_FLAG_RXNE) != RESET)
      {
        uint8_t byte = (uint8_t)(uart->Instance->DR & 0xFFU);
        frame_result_t result = parser_feed(&detection_parsers[port], byte, frame);
        if (result != FRAME_INCOMPLETE)
        {
          boot_uart = uart;
          return result;
        }
      }
    }

    if ((timeout != HAL_MAX_DELAY) && ((HAL_GetTick() - start) >= timeout))
    {
      return FRAME_TIMEOUT;
    }
  }
}

static bool consume_boot_request(void)
{
  bool requested;

  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
  requested = (RTC->BKP0R == BOOT_REQUEST_MAGIC) &&
              (RTC->BKP1R == ~BOOT_REQUEST_MAGIC);
  if (requested)
  {
    RTC->BKP0R = 0U;
    RTC->BKP1R = 0U;
  }
  HAL_PWR_DisableBkUpAccess();
  return requested;
}

static void send_response(uint8_t command, uint16_t sequence, uint8_t result,
                          uint16_t expected_sequence)
{
  uint8_t frame[16];
  const uint16_t payload_length = 6U;
  const uint8_t message_type = (result == BOOT_RESULT_OK) ? BOOT_MSG_ACK : BOOT_MSG_NACK;

  frame[0] = BOOT_MAGIC_0;
  frame[1] = BOOT_MAGIC_1;
  frame[2] = BOOT_PROTOCOL_VERSION;
  frame[3] = message_type;
  put_u16_le(&frame[4], sequence);
  put_u16_le(&frame[6], payload_length);
  put_u16_le(&frame[8], sequence);
  frame[10] = command;
  frame[11] = result;
  put_u16_le(&frame[12], expected_sequence);
  put_u16_le(&frame[14], crc16_modbus(&frame[2], 12U));
  if (boot_uart != NULL)
  {
    (void)HAL_UART_Transmit(boot_uart, frame, sizeof(frame), BOOT_FRAME_TIMEOUT_MS);
  }
}

static bool app_vector_is_valid(uint32_t image_length)
{
  uint32_t stack = *(volatile const uint32_t *)APP_START_ADDRESS;
  uint32_t reset = *(volatile const uint32_t *)(APP_START_ADDRESS + 4U);
  uint32_t reset_address = reset & ~1UL;
  bool stack_in_main_sram = (stack > MAIN_SRAM_START) && (stack <= MAIN_SRAM_END);
  bool stack_in_ccm_sram = (stack > CCM_SRAM_START) && (stack <= CCM_SRAM_END);

  if ((image_length < 8U) || (image_length > APP_MAX_IMAGE_SIZE))
  {
    return false;
  }
  if (((stack & 7UL) != 0U) || (!stack_in_main_sram && !stack_in_ccm_sram))
  {
    return false;
  }
  if (((reset & 1UL) == 0U) || (reset_address < APP_START_ADDRESS) ||
      (reset_address >= (APP_START_ADDRESS + image_length)))
  {
    return false;
  }
  return true;
}

static bool app_image_is_valid(void)
{
  const volatile app_metadata_t *metadata =
      (const volatile app_metadata_t *)APP_METADATA_ADDRESS;

  if ((metadata->magic != APP_METADATA_MAGIC) ||
      (metadata->magic_inverse != ~APP_METADATA_MAGIC) ||
      (metadata->crc_inverse != ~metadata->image_crc32) ||
      !app_vector_is_valid(metadata->image_length))
  {
    return false;
  }
  return crc32_ieee((const uint8_t *)APP_START_ADDRESS, metadata->image_length) ==
         metadata->image_crc32;
}

static bool erase_app_sectors(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0U;
  HAL_StatusTypeDef status;

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector = FLASH_SECTOR_4;
  /* 只擦 4~9（0x08010000..0x080BFFFF，共 704KB = APP 链接脚本的 FLASH LENGTH）。
   * 必须**止步于 Sector 9**：Sector 10 (0x080C0000) 是 APP 的持久化配置/SOC 追加区
   * （APP_CONFIG_ADDRESS，见 bms_config.h），之前连擦到 Sector 11 会把 total_ic、
   * 保护阈值和 SOC 一起抹掉，每次 OTA 后静默回默认值。
   * Sector 11 (0x080FFF00 元数据) 也不在擦除范围：由 write_app_metadata() 显式改写，
   * 且旧的 image_crc32 对不上已被擦成 0xFF 的镜像，启动校验自然失败。 */
  erase.NbSectors = 6U;
  status = HAL_FLASHEx_Erase(&erase, &sector_error);
  (void)HAL_FLASH_Lock();
  return (status == HAL_OK) && (sector_error == 0xFFFFFFFFUL);
}

static bool program_flash_bytes(uint32_t address, const uint8_t *data, uint32_t length)
{
  uint32_t end = address + length;

  if ((length == 0U) || (address < APP_START_ADDRESS) || (end < address) ||
      (end > APP_CONFIG_ADDRESS))
  {
    return false;
  }
  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }

  for (uint32_t index = 0U; index < length; ++index)
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, address + index, data[index]) != HAL_OK)
    {
      (void)HAL_FLASH_Lock();
      return false;
    }
  }
  (void)HAL_FLASH_Lock();
  return memcmp((const void *)address, data, length) == 0;
}

static bool write_app_metadata(uint32_t image_length, uint32_t image_crc32)
{
  uint32_t values[5] = {
      APP_METADATA_MAGIC,
      ~APP_METADATA_MAGIC,
      image_length,
      image_crc32,
      ~image_crc32,
  };

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return false;
  }

  /* Write the magic last: a reset at any earlier point leaves the APP invalid. */
  for (uint32_t index = 1U; index < 5U; ++index)
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          APP_METADATA_ADDRESS + index * sizeof(uint32_t), values[index]) != HAL_OK)
    {
      (void)HAL_FLASH_Lock();
      return false;
    }
  }
  if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, APP_METADATA_ADDRESS, values[0]) != HAL_OK)
  {
    (void)HAL_FLASH_Lock();
    return false;
  }
  (void)HAL_FLASH_Lock();
  return true;
}

static void jump_to_app(void)
{
  uint32_t app_stack = *(volatile const uint32_t *)APP_START_ADDRESS;
  uint32_t app_reset = *(volatile const uint32_t *)(APP_START_ADDRESS + 4U);

  __disable_irq();
  (void)HAL_UART_DeInit(&huart1);
  (void)HAL_UART_DeInit(&huart3);
  (void)HAL_CRC_DeInit(&hcrc);
  (void)HAL_DeInit();
  (void)HAL_RCC_DeInit();

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

  for (uint32_t index = 0U; index < 8U; ++index)
  {
    NVIC->ICER[index] = 0xFFFFFFFFUL;
    NVIC->ICPR[index] = 0xFFFFFFFFUL;
  }

  SCB->VTOR = APP_START_ADDRESS;
  __set_CONTROL(0U);
  __set_PSP(0U);
  __DSB();
  __ISB();

  /* Do not execute C code after changing MSP: the compiler may use the old frame. */
  __asm volatile(
      "msr msp, %0\n"
      "cpsie i\n"
      "bx %1\n"
      :
      : "r"(app_stack), "r"(app_reset)
      : "memory");
  __builtin_unreachable();
}

static void run_upgrade_session(uint16_t expected_sequence, uint16_t hello_sequence)
{
  enum { WAIT_BEGIN, RECEIVE_IMAGE, IMAGE_READY } state = WAIT_BEGIN;
  uint16_t last_acked_sequence = hello_sequence;
  uint8_t last_acked_command = BOOT_CMD_HELLO;
  uint32_t expected_length = 0U;
  uint32_t expected_crc32 = 0U;
  uint32_t bytes_written = 0U;
  uint32_t image_ready_started = 0U;

  for (;;)
  {
    uint32_t idle_timeout = BOOT_SESSION_IDLE_MS;
    if (state == IMAGE_READY)
    {
      uint32_t ready_elapsed = HAL_GetTick() - image_ready_started;
      if (ready_elapsed >= BOOT_IMAGE_READY_MS)
      {
        jump_to_app();
      }
      idle_timeout = BOOT_IMAGE_READY_MS - ready_elapsed;
    }
    frame_result_t frame_status = receive_frame(boot_uart, &rx_frame, idle_timeout);

    if (frame_status == FRAME_TIMEOUT)
    {
      if (state == IMAGE_READY || ((state == WAIT_BEGIN) && app_image_is_valid()))
      {
        jump_to_app();
      }

      /* APP 已被擦除时返回握手阶段，允许主机重新建立完整升级会话。 */
      return;
    }

    if (frame_status == FRAME_BAD_CRC)
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_CRC,
                    expected_sequence);
      continue;
    }
    if (frame_status == FRAME_BAD_LENGTH)
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_LENGTH,
                    expected_sequence);
      continue;
    }
    if (frame_status != FRAME_OK)
    {
      continue;
    }

    if ((rx_frame.sequence == last_acked_sequence) &&
        (rx_frame.command == last_acked_command))
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_OK,
                    expected_sequence);
      continue;
    }
    if (rx_frame.sequence != expected_sequence)
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_SEQ,
                    expected_sequence);
      continue;
    }

    uint8_t result = BOOT_RESULT_OK;
    switch (rx_frame.command)
    {
      case BOOT_CMD_BEGIN:
        if (rx_frame.length != 8U)
        {
          result = BOOT_RESULT_BAD_LENGTH;
          break;
        }
        expected_length = get_u32_le(&rx_frame.payload[0]);
        expected_crc32 = get_u32_le(&rx_frame.payload[4]);
        if ((expected_length < 8U) || (expected_length > APP_MAX_IMAGE_SIZE))
        {
          result = BOOT_RESULT_BAD_LENGTH;
          break;
        }
        if (!erase_app_sectors())
        {
          state = WAIT_BEGIN;
          result = BOOT_RESULT_FLASH;
          break;
        }
        bytes_written = 0U;
        state = RECEIVE_IMAGE;
        break;

      case BOOT_CMD_DATA:
      {
        if (state != RECEIVE_IMAGE)
        {
          result = BOOT_RESULT_BAD_STATE;
          break;
        }
        if (rx_frame.length <= 4U)
        {
          result = BOOT_RESULT_BAD_LENGTH;
          break;
        }
        uint32_t offset = get_u32_le(&rx_frame.payload[0]);
        uint32_t data_length = rx_frame.length - 4U;
        if ((offset != bytes_written) || (data_length > (expected_length - bytes_written)))
        {
          result = BOOT_RESULT_BAD_LENGTH;
          break;
        }
        if (!program_flash_bytes(APP_START_ADDRESS + offset, &rx_frame.payload[4], data_length))
        {
          state = WAIT_BEGIN;
          result = BOOT_RESULT_FLASH;
          break;
        }
        bytes_written += data_length;
        break;
      }

      case BOOT_CMD_END:
        if ((state != RECEIVE_IMAGE) || (rx_frame.length != 0U))
        {
          result = BOOT_RESULT_BAD_STATE;
          break;
        }
        if (bytes_written != expected_length)
        {
          result = BOOT_RESULT_BAD_LENGTH;
          break;
        }
        if (crc32_ieee((const uint8_t *)APP_START_ADDRESS, expected_length) != expected_crc32)
        {
          state = WAIT_BEGIN;
          result = BOOT_RESULT_IMAGE_CRC;
          break;
        }
        if (!app_vector_is_valid(expected_length))
        {
          state = WAIT_BEGIN;
          result = BOOT_RESULT_VECTOR;
          break;
        }
        if (!write_app_metadata(expected_length, expected_crc32) || !app_image_is_valid())
        {
          state = WAIT_BEGIN;
          result = BOOT_RESULT_FLASH;
          break;
        }
        state = IMAGE_READY;
        image_ready_started = HAL_GetTick();
        break;

      case BOOT_CMD_RUN:
        if ((state != IMAGE_READY) || (rx_frame.length != 0U))
        {
          result = BOOT_RESULT_BAD_STATE;
          break;
        }
        send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_OK,
                      (uint16_t)(expected_sequence + 1U));
        HAL_Delay(20U);
        jump_to_app();
        break;

      default:
        result = BOOT_RESULT_BAD_COMMAND;
        break;
    }

    if (result == BOOT_RESULT_OK)
    {
      last_acked_sequence = rx_frame.sequence;
      last_acked_command = rx_frame.command;
      ++expected_sequence;
    }
    /* A successful response advertises the sequence number of the next command. */
    send_response(rx_frame.command, rx_frame.sequence, result, expected_sequence);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CRC_Init();
  MX_USART3_UART_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  boot_uart = NULL;
  bool force_upgrade = consume_boot_request();
  bool valid_app = app_image_is_valid();
  uint32_t wait_started = HAL_GetTick();

  HAL_GPIO_WritePin(STATUS_6_GPIO_Port, STATUS_6_Pin, GPIO_PIN_SET);
  
  for (;;)
  {
    uint32_t timeout = HAL_MAX_DELAY;
    if (valid_app && !force_upgrade)
    {
      uint32_t elapsed = HAL_GetTick() - wait_started;
      if (elapsed >= BOOT_WAIT_MS)
      {
        jump_to_app();
      }
      timeout = BOOT_WAIT_MS - elapsed;
    }

    frame_result_t frame_status = receive_frame_any(&rx_frame, timeout);
    if (frame_status == FRAME_TIMEOUT)
    {
      if (valid_app && !force_upgrade)
      {
        jump_to_app();
      }
      continue;
    }
    if (frame_status == FRAME_BAD_CRC)
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_CRC, 0U);
      continue;
    }
    if (frame_status == FRAME_BAD_LENGTH)
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_LENGTH, 0U);
      continue;
    }
    if ((rx_frame.command != BOOT_CMD_HELLO) || (rx_frame.length != 0U))
    {
      send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_BAD_COMMAND, 0U);
      continue;
    }

    uint16_t expected_sequence = (uint16_t)(rx_frame.sequence + 1U);
    send_response(rx_frame.command, rx_frame.sequence, BOOT_RESULT_OK, expected_sequence);
    run_upgrade_session(expected_sequence, rx_frame.sequence);
    valid_app = app_image_is_valid();
    force_upgrade = false;
    wait_started = HAL_GetTick();
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 64;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(PRECHARGE_CTRL_GPIO_Port, PRECHARGE_CTRL_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, NEG_AIR_CTRL_Pin|POS_AIR_CTRL_Pin|STATUS_6_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : PRECHARGE_CTRL_Pin */
  GPIO_InitStruct.Pin = PRECHARGE_CTRL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PRECHARGE_CTRL_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : NEG_AIR_CTRL_Pin POS_AIR_CTRL_Pin */
  GPIO_InitStruct.Pin = NEG_AIR_CTRL_Pin|POS_AIR_CTRL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : STATUS_6_Pin */
  GPIO_InitStruct.Pin = STATUS_6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(STATUS_6_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
