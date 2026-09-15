/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */
extern int sdc_closed;   /* BMS: 高压 SDC 回路闭合状态，由 bms_io 注入 */
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED0_Pin GPIO_PIN_3
#define LED0_GPIO_Port GPIOE
#define LED1_Pin GPIO_PIN_4
#define LED1_GPIO_Port GPIOE
#define LED2_Pin GPIO_PIN_9
#define LED2_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */
/* ---- BMS 信号引脚映射（自 BMS_V1.0 迁入）。本工程仅做逻辑验证：IO 走 bms_io 虚拟层，
 *      输出=串口打印，输入=串口注入，不实际驱动引脚（故与 LED/USART 同脚也不冲突）---- */
#define BMS_ERROR_Pin GPIO_PIN_2
#define BMS_ERROR_GPIO_Port GPIOE
#define SDC_VOLTAGE_Pin GPIO_PIN_3
#define SDC_VOLTAGE_GPIO_Port GPIOE
#define CHARGE_ENABLE_Pin GPIO_PIN_4
#define CHARGE_ENABLE_GPIO_Port GPIOE
#define POWER_UP_Pin GPIO_PIN_5
#define POWER_UP_GPIO_Port GPIOE
#define HV_MISMATCH_Pin GPIO_PIN_4
#define HV_MISMATCH_GPIO_Port GPIOC
#define BMS_NERROR_Pin GPIO_PIN_0
#define BMS_NERROR_GPIO_Port GPIOB
#define ERROR_LED_Pin GPIO_PIN_8
#define ERROR_LED_GPIO_Port GPIOE
#define HV_ACTIVE_Pin GPIO_PIN_10
#define HV_ACTIVE_GPIO_Port GPIOE
#define PRECHARGE_CTRL_Pin GPIO_PIN_15
#define PRECHARGE_CTRL_GPIO_Port GPIOB
#define NEG_AIR_CTRL_Pin GPIO_PIN_9
#define NEG_AIR_CTRL_GPIO_Port GPIOD
#define POS_AIR_CTRL_Pin GPIO_PIN_11
#define POS_AIR_CTRL_GPIO_Port GPIOD
#define STATUS_6_Pin GPIO_PIN_13
#define STATUS_6_GPIO_Port GPIOD
#define STATUS_5_Pin GPIO_PIN_14
#define STATUS_5_GPIO_Port GPIOD
#define STATUS_4_Pin GPIO_PIN_15
#define STATUS_4_GPIO_Port GPIOD
#define STATUS_3_Pin GPIO_PIN_6
#define STATUS_3_GPIO_Port GPIOC
#define STATUS_2_Pin GPIO_PIN_7
#define STATUS_2_GPIO_Port GPIOC
#define STATUS_1_Pin GPIO_PIN_8
#define STATUS_1_GPIO_Port GPIOC
#define SPI3_CS_Pin GPIO_PIN_0
#define SPI3_CS_GPIO_Port GPIOD
#define SPI1_CS_Pin GPIO_PIN_6
#define SPI1_CS_GPIO_Port GPIOB
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
