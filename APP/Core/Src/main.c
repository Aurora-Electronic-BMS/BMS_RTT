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
#include "can.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bms_io.h"
#include "bms_config.h"
#include "can_custom.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "soc_estimation.h"
#include "state_machine.h"
#include "telemetry_serial.h"
#include "LTC68041.h"
#include <rtthread.h>

#include <stdio.h>
#include <math.h>
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* sdc_closed 定义在 bms_io.c（虚拟 IO 层持有，可被串口注入覆盖） */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void bms_app_start(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* CAN 波特率取自 bms_config，覆盖 MX_CANx_Init 里 CubeMX 生成的固定预分频。
 * 位时间分段固定 BS1=13TQ/BS2=2TQ/SJW=1TQ（共 16TQ），速率只由预分频决定：
 * PCLK1 32MHz/(16×4)=500k、/(16×8)=250k，仅支持配置校验过的两档。
 * 放在 USER CODE 区而非 can.c 的生成区：重新生成会覆盖那边的手改。
 * 改波特率需重启后生效（不做热切换）。 */
static void can_apply_config_baudrate(void)
{
  uint32_t want1 = (bms_config.can1_baudrate == 250000) ? 8U : 4U;
  uint32_t want2 = (bms_config.can2_baudrate == 250000) ? 8U : 4U;

  /* 只在与生成值不同时重跑 HAL_CAN_Init（State==READY 会跳过 MspInit，安全） */
  if (hcan1.Init.Prescaler != want1) {
    hcan1.Init.Prescaler = want1;
    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
      Error_Handler();
    }
  }
  if (hcan2.Init.Prescaler != want2) {
    hcan2.Init.Prescaler = want2;
    if (HAL_CAN_Init(&hcan2) != HAL_OK) {
      Error_Handler();
    }
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
  /* rt_hw_board_init() 在 PLL 启动前按 HSI 频率装了 SysTick LOAD；现在时钟
   * 已升到目标频率，按实际 SystemCoreClock 重装，否则 RT tick 会按倍率偏快，
   * 所有 rt_thread_mdelay 都缩水。SysTick 是 RTOS 时基，TIM7(HAL tick)不受影响。 */
  SysTick->LOAD = (uint32_t)(SystemCoreClock / RT_TICK_PER_SECOND) - 1UL;
  SysTick->VAL  = 0UL;
  SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;

  /* 配置必须先于 CAN 初始化：can_apply_config_baudrate 的预分频取自 bms_config 的
   * 波特率，且 bms_config_init 内部会按配置刷新分流器/AFE 的阈值与去抖时间，并置
   * flash_layout_ready（决定 SOC/配置能否落盘）。漏掉这一句时 bms_config 恒为 0（BSS）、
   * 8 个监视器 delay_ms=0，上电首拍即判故障 → 恒 TS_ERROR、AIR 不闭合。
   * 放在 USER CODE 区（而非生成区）以免 CubeMX 重新生成时丢失；与裸机版同序
   * （BMS_Bare main.c:128-131）。 */
  bms_config_init();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN1_Init();
  MX_CAN2_Init();
  MX_SPI1_Init();
  MX_SPI3_Init();
  MX_USART3_UART_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* CAN 外设已就绪，按 bms_config 覆盖生成时的缺省波特率（配置在 SysInit 已加载）。 */
  can_apply_config_baudrate();
  /* 启动 BMS 应用：CAN 过滤器/缓冲、状态机、双口遥测(含 0x13 重入)，
   * 并创建 can_rx / ctrl / telemetry 三个线程。 */
  bms_app_start();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 让出 CPU：main 线程优先级(10)高于 LED 心跳线程(15)，忙等会饿死它 */
    rt_thread_mdelay(1000);
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
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM7 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM7)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
    HAL_GPIO_WritePin(ERROR_LED_GPIO_Port, ERROR_LED_Pin, GPIO_PIN_SET);
    ts_sm_ctrl_AIR_pwr(AIR_POS, 0);
    ts_sm_ctrl_AIR_pwr(AIR_NEG, 0);
    ts_sm_ctrl_AIR_pwr(AIR_PRE, 0);
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
