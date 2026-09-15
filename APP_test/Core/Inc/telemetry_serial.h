#ifndef INC_TELEMETRY_SERIAL_H
#define INC_TELEMETRY_SERIAL_H

/* ==========================================================================
 * telemetry_serial.h —— B5 4D 二进制协议对外接口
 * 帧格式、链路模型与收发模型见 telemetry_serial.c 头部。
 * ========================================================================== */

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 初始化两个串口（都能跑本协议），开逐字节接收中断 */
void telemetry_serial_init(UART_HandleTypeDef *uart1, UART_HandleTypeDef *uart2);

/* 排空 RX 环并喂拼帧状态机（线程侧） */
void telemetry_serial_process(void);

/* 到 200ms 上报点且链路活跃时发一帧全量遥测 */
void telemetry_serial_send_periodic(void);

/* 事件驱动：等串口 RX 中断；上位机在线时超时=距下次 200ms 上报的剩余时间 */
void telemetry_serial_wait_next(void);

/* 上位机链路是否活跃（活跃时避免打印文本，保持二进制流干净） */
int telemetry_host_active(void);

#endif
