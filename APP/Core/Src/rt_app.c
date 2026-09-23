/**
 * rt_app.c —— BMS_RTT 的 RT-Thread 控制台出口 + LED 心跳线程
 *
 * 0x13 ENTER_BOOTLOADER 重入已由 telemetry_serial.c 的 B5 4D 协议处理
 * （MSG_ENTER_BOOTLOADER：写 RTC->BKP0R/BKP1R 后 NVIC_SystemReset），
 * USART1/USART3 两个口都能触发，因此这里不再重复实现。
 *
 * 本文件只做两件事：把 rt_kprintf 的字符出口接到 USART1，以及开一个心跳线程。
 */
#include "main.h"
#include "usart.h"
#include "gpio.h"
#include "bms_io.h"
#include <rtthread.h>
#include <rthw.h>
#include <stdarg.h>

/* ====================================================================
 *  ① 串口打印函数：让 rt_kprintf 输出到 USART1 (PA9/PA10)
 *     与遥测发送共用一把 TX 锁，避免与 B5 4D 二进制帧在串口上交错
 * ==================================================================== */
void rt_hw_console_output(const char *str)
{
  /* 串口还没初始化（Instance 为 NULL）时丢弃早期打印。
   * 不能按 gState!=READY 丢弃——并发打印时别的线程正在发，会把整行吞掉；
   * 并发由下面的 bms_tx_lock 串行化，HAL_UART_Transmit 在锁内必然 READY */
  if (huart1.Instance == RT_NULL)
    return;
  if ((str != RT_NULL) && (str[0] != '\0'))
  {
    bms_tx_lock();
    HAL_UART_Transmit(&huart1, (const uint8_t *)str, (uint16_t)rt_strlen(str), 200U);
    bms_tx_unlock();
  }
}

/* ====================================================================
 *  ② 心跳线程（STATUS_1 = PC8）
 *     50ms 翻转，与裸机版主循环（LOOP_PERIOD 50ms 里 TogglePin STATUS_1）
 *     同节拍，两版肉眼观感一致；也用于确认 RTOS 仍在调度（没死锁/没饿死）。
 *     状态机相关指示（STATUS_3 非 INACTIVE / STATUS_6 有故障）在 bms_ctrl 线程里。
 * ==================================================================== */
static struct rt_thread led_thread;
static rt_uint8_t led_stack[512];

static void led_thread_entry(void *param)
{
  (void)param;
  rt_kprintf("[APP] LED thread started\r\n");
  while (1)
  {
    HAL_GPIO_TogglePin(STATUS_1_GPIO_Port, STATUS_1_Pin);
    rt_thread_mdelay(50);
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
