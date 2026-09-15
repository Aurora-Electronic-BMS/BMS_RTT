#ifndef INC_TELEMETRY_SERIAL_H
#define INC_TELEMETRY_SERIAL_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

void telemetry_serial_init(UART_HandleTypeDef *uart1, UART_HandleTypeDef *uart2);
void telemetry_serial_process(void);
void telemetry_serial_send_periodic(void);

/* 是否有活跃上位机会话（收到心跳后为真，超时自动清零）。供日志/状态行判断。 */
int telemetry_host_active(void);

#ifdef BMS_USE_RTTHREAD
/* 仅 RTT 版：遥测线程睡到"来字节"或"距下次上报点"较早者（裸机主循环轮询，不用）。 */
void telemetry_serial_wait_next(void);
#endif

#endif
