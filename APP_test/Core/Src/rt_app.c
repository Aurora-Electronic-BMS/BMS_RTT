/**
 * rt_app.c —— APP_test 应用代码（RT-Thread 控制台 + LED 线程）
 *
 * 0x13 ENTER_BOOTLOADER 重入已由 telemetry_serial.c 的 B5 4D 协议处理
 * （MSG_ENTER_BOOTLOADER：写 RTC->BKP0R/BKP1R 后 NVIC_SystemReset），
 * UART5 / USART6 两个口都能触发，因此这里不再重复实现。
 *
 * 本文件只做两件事：把 rt_kprintf 的字符出口接到 USART6，以及开一个 LED0 心跳线程。
 */
#include "main.h"
#include "usart.h"
#include "gpio.h"
#include "bms_io.h"
#include <rtthread.h>
#include <rthw.h>
#include <stdarg.h>

/* ====================================================================
 *  ① 串口打印函数：让 rt_kprintf 输出到 USART6 (PC6/PC7)
 *     与遥测发送共用一把 TX 锁，避免与 B5 4D 二进制帧在串口上交错
 * ==================================================================== */
void rt_hw_console_output(const char *str)
{
  /* UART not initialized yet (Instance still NULL): drop early prints.
   * 不能按 gState!=READY 丢弃——并发打印时别的线程正在发，会把整行吞掉；
   * 并发由下面的 bms_tx_lock 串行化，HAL_UART_Transmit 在锁内必然 READY */
  if (huart6.Instance == RT_NULL)
    return;
  if ((str != RT_NULL) && (str[0] != '\0'))
  {
    bms_tx_lock();
    HAL_UART_Transmit(&huart6, (const uint8_t *)str, (uint16_t)rt_strlen(str), 200U);
    bms_tx_unlock();
  }
}

/* ====================================================================
 *  ② LED 心跳线程（LED0 = PE3；LED1/LED2 由 bms_ctrl 线程驱动）
 *     最慢的 500ms 翻转，用于肉眼确认 RTOS 仍在调度（没死锁/没饿死）。
 * ==================================================================== */
static struct rt_thread led_thread;
static rt_uint8_t led_stack[512];

static void led_thread_entry(void *param)
{
  (void)param;
  rt_kprintf("[APP] LED thread started\r\n");
  while (1)
  {
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
    rt_thread_mdelay(500);
  }
}

/* ====================================================================
 *  ③ 应用初始化：自动创建线程
 *     INIT_APP_EXPORT 段在 RT-Thread 初始化阶段自动执行（早于 main 里的
 *     bms_app_start），此处只建线程不碰外设；优先级 15（最低）够用。
 * ==================================================================== */
static int rt_app_init(void)
{
  rt_thread_init(&led_thread, "led_blink", led_thread_entry, RT_NULL,
                 led_stack, sizeof(led_stack), 15, 10);
  rt_thread_startup(&led_thread);

  rt_kprintf("[APP] RT-Thread initialized\r\n");
  return 0;
}
INIT_APP_EXPORT(rt_app_init);
