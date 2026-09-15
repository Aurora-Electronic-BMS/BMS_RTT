/* bms_app.c —— 把 BMS 应用层接到 RT-Thread 上（BMS_RTT）
 *
 * 业务模块(state_machine / shunt_monitor / slave_monitor / can_custom /
 * telemetry_serial)与 BMS_Bare 裸机版共用同一份源码，这里只负责把它们拆成
 * 三个 RT-Thread 线程：
 *   bms_can_rx     信号量驱动：排空 CAN 环形缓冲并分发(shunt/charge/drive)
 *   bms_ctrl       50ms 固定周期：状态机每拍跑；电芯/分流/上报 150ms 分频
 *   bms_telemetry  事件驱动：串口 RX 中断唤醒收帧，超时兜底 200ms 周期上报
 *
 * 线程在 main() 的外设初始化之后、由 bms_app_start() 创建并启动。
 * 优先级（数值越小越高）：can_rx=7 > ctrl=8 > tele=9，均高于 main 线程(10)。
 */
#include <rtthread.h>
#include "bms_io.h"
#include "can.h"
#include "can_custom.h"
#include "LTC68041.h"
#include "main.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "soc_estimation.h"
#include "state_machine.h"
#include "telemetry_serial.h"
#include "usart.h"

#define BMS_CTRL_PERIOD_MS 50
#define BMS_HEAVY_DIV 3                     /* 50ms * 3 = 150ms 跑一遍重活 */

/* ---- CAN RX 线程 ---- */
static struct rt_thread can_rx_thread;
static rt_uint8_t can_rx_stack[1024];
static struct rt_semaphore can_rx_sem;      /* ISR 释放、线程 take，用于唤醒出队 */

/* ---- 控制线程 ---- */
static struct rt_thread ctrl_thread;
static rt_uint8_t ctrl_stack[1536];

/* ---- 遥测线程 ---- */
static struct rt_thread tele_thread;
static rt_uint8_t tele_stack[1280];

/* 由 can_custom.c 的 ISR 回调调用：每入队一帧就释放一次信号量。
 * ISR 里只做 rt_sem_release（RT-Thread 保证其 ISR 安全）。 */
void bms_can_rx_notify(void) {
    rt_sem_release(&can_rx_sem);
}

/* CAN RX 线程主体：等信号量 → 排空环形缓冲 → 逐帧分发。
 * 用 while 排空（而非每信号量一帧）以便消息风暴时一次清空积压，减少切换；
 * 分发在**线程上下文**做，ISR 只负责入队。 */
static void bms_can_rx_entry(void *parameter) {
    bms_can_msg_t msg;
    (void)parameter;

    bms_log("[BMS] can_rx thread up\r\n");

    for(;;) {
        rt_sem_take(&can_rx_sem, RT_WAITING_FOREVER);
        while(can_custom_msg_pop(&msg)) {
            can_custom_dispatch(&msg);
        }
    }
}

/* 固定周期推进。rt_thread_delay_until 是 RT-Thread 4.0 的 API，3.1.3 没有，
 * 这里手写等价物：落后(remain<=0)就跳过重锚，不追补，避免积压后突发。
 * 顺带驱动 STATUS_2：与裸机版 loop_delay() 一致，本拍超支点亮点、未超支熄灭。 */
static void bms_delay_until(rt_tick_t *next, rt_tick_t period)
{
    rt_int32_t remain = (rt_int32_t)(*next - rt_tick_get());

    if (remain > 0) {
        rt_thread_delay((rt_tick_t)remain);
        *next += period;
        HAL_GPIO_WritePin(STATUS_2_GPIO_Port, STATUS_2_Pin, GPIO_PIN_RESET);
    }
    else {
        *next = rt_tick_get() + period;
        HAL_GPIO_WritePin(STATUS_2_GPIO_Port, STATUS_2_Pin, GPIO_PIN_SET);
    }
}

/* 控制线程主体：50ms 一拍。重活（电芯采样+分流检查+CAN 状态帧+回环自测）
 * 每 3 拍一次（150ms），轻活（状态机推进）每拍都做。 */
static void bms_ctrl_entry(void *parameter) {
    rt_uint8_t last_state = 0xFFU;      /* 初值 0xFF 保证首拍必打一次 [SM] */
    uint32_t last_status = 0;
    rt_uint8_t heavy_div = 0;
    rt_tick_t next = rt_tick_get();
    (void)parameter;

    bms_log("[BMS] ctrl thread up\r\n");

    for(;;) {
        rt_uint8_t heavy = 0U;

        bms_delay_until(&next, rt_tick_from_millisecond(BMS_CTRL_PERIOD_MS));

        /* 本拍输入：SDC 读实际引脚电平（未注入时）——与裸机版主循环同一位置同一语义。
         * 少这一行，状态机在 RTT 里永远看到 sdc_closed=1（bms_io.c 的初值），
         * 即 SDC 断开时不会转 DISCHARGE/停发充电请求。 */
        sdc_closed = (bms_in(SDC_VOLTAGE_GPIO_Port, SDC_VOLTAGE_Pin) == GPIO_PIN_SET) ? 1 : 0;

        if(++heavy_div >= BMS_HEAVY_DIV) {
            heavy_div = 0U;
            heavy = 1U;
        }

        /* 保持与裸机版相同的相对顺序：先采电芯/分流，再跑状态机，最后上报 */
        if(heavy) {
            slaves_check();
            shunt_check();
            /* SOC 估读取分流器累计电量与 min_voltage，写 current_soc；
             * 遥测线程也读 current_soc，故与其它共享状态同锁保护 */
            bms_state_lock();
            soc_update();
            bms_state_unlock();
        }

        ts_sm_update();

        if(heavy) {
            can_send_status();
            can_custom_selftest_tick();
        }

        /* 状态变化才打印一行 [SM]，避免每拍刷屏。ctrl=%u 是当前控制模式
         * (0=GPIO 1=串口目标 2=手动 AIR)，air=0x%X 是 AIR 实际引脚回读。 */
        if((rt_uint8_t)ts_state.current_state != last_state) {
            last_state = (rt_uint8_t)ts_state.current_state;
            bms_log("[SM] state=%u target=%u err=0x%X air=0x%X ctrl=%u\r\n",
                    (unsigned)ts_state.current_state,
                    (unsigned)ts_state.target_state,
                    (unsigned)ts_error_kind,
                    (unsigned)ts_sm_get_air_bits(),
                    (unsigned)ts_sm_get_control_mode());
        }

        /* 上位机没连时每秒打一行状态，方便纯串口逻辑验证；
         * 上位机连着时不打，保持 B5 4D 二进制流干净 */
        if(!telemetry_host_active() &&
           (HAL_GetTick() - last_status >= 1000U)) {
            last_status = HAL_GetTick();
            /* SOC 用整数百分比打印：newlib-nano 未链接 _printf_float，%f 打不出来 */
            bms_log("[ST] sm=%u tgt=%u err=0x%X air=0x%X vbat=%ld vbat_sh=%ld i=%ld soc=%u%% minV=%u maxV=%u minT=%d maxT=%d\r\n",
                    (unsigned)ts_state.current_state,
                    (unsigned)ts_state.target_state,
                    (unsigned)ts_error_kind,
                    (unsigned)ts_sm_get_air_bits(),
                    (long)Bat_Volt,
                    (long)shunt_data.voltage_bat,
                    (long)shunt_data.current,
                    (unsigned)(current_soc + 0.5f),
                    (unsigned)min_voltage,
                    (unsigned)max_voltage,
                    (int)min_temp,
                    (int)max_temp);
        }

        /* 状态指示与裸机版主循环一一对应：
         *   STATUS_1 RTOS 心跳(rt_app.c 的 LED 线程) / STATUS_2 本拍超支(bms_delay_until)
         *   STATUS_3 状态机非 INACTIVE / STATUS_6 有故障
         *   STATUS_5 启动时 CAN 初始化失败(bms_app_start) */
        HAL_GPIO_WritePin(STATUS_3_GPIO_Port, STATUS_3_Pin,
                          (ts_state.current_state == TS_INACTIVE) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(STATUS_6_GPIO_Port, STATUS_6_Pin,
                          ts_state.error ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

/* 遥测线程主体：事件驱动 + 超时兜底。
 * telemetry_serial_wait_next() 睡到"串口来字节"或"距下次 200ms 上报点"两者较早者；
 * 醒来后 process 收帧、send_periodic 到点则发一帧遥测。 */
static void bms_telemetry_entry(void *parameter) {
    (void)parameter;

    bms_log("[BMS] telemetry thread up\r\n");

    for(;;) {
        telemetry_serial_wait_next();
        telemetry_serial_process();
        telemetry_serial_send_periodic();
    }
}

/* INIT_APP_EXPORT：在 main() 之前跑，只建互斥锁，不碰外设 */
static int bms_app_init(void) {
    bms_io_init();
    return 0;
}
INIT_APP_EXPORT(bms_app_init);

/* 由 main() 在外设 MX_*_Init 之后调用：初始化各业务模块，再创建并启动三个线程。 */
void bms_app_start(void) {
    can_rx_pullup();        /* can_custom.c 提供：无收发器时给 CAN_RX 内部上拉 */

    /* 信号量必须先于 can_init：CAN 中断可能在使能后立刻回调 */
    rt_sem_init(&can_rx_sem, "canrx", 0, RT_IPC_FLAG_FIFO);

    /* 与裸机版 main.c 一致：CAN 起不来点亮 STATUS_5（裸机版同样不 Error_Handler） */
    if (can_init() != HAL_OK) {
        HAL_GPIO_WritePin(STATUS_5_GPIO_Port, STATUS_5_Pin, GPIO_PIN_SET);
    }
    slaves_init();
    shunt_init();
    soc_init();
    ts_sm_init();

    /* 收窄 CAN/UART 中断优先级(覆盖 CubeMX 生成的 0)：避免消息风暴时
     * 长时间压住 SysTick(最低优先级)导致 RT-Thread 调度停摆 */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5, 0);
    HAL_NVIC_SetPriority(CAN2_RX0_IRQn, 5, 0);
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);

    /* 两个串口都能跑 B5 4D 协议(含 0x13 重入升级) */
    telemetry_serial_init(&huart2, &huart3);

    /* 真 AFE 的 set_adc() 在此配 ADCV/ADAX 命令字节，漏掉则转换读回来全是垃圾。
     * 桩件版的 LTC6804_initialize() 是空函数，两边共用同一次调用。 */
    LTC6804_initialize();

    /* 与裸机版 main.c 一致：启动即把"无故障"指示拉高（gpio.c 初值是低；
     * INACTIVE 态每拍会重写，但首拍之前这一段要保持与裸机同电平）。 */
    bms_out(BMS_NERROR_GPIO_Port, BMS_NERROR_Pin, GPIO_PIN_SET);

    /* 外设已就绪，此时才创建并启动线程。 */
    rt_thread_init(&can_rx_thread, "bms_can_rx", bms_can_rx_entry, RT_NULL,
                   can_rx_stack, sizeof(can_rx_stack), 7, 10);
    rt_thread_init(&ctrl_thread, "bms_ctrl", bms_ctrl_entry, RT_NULL,
                   ctrl_stack, sizeof(ctrl_stack), 8, 10);
    rt_thread_init(&tele_thread, "bms_tele", bms_telemetry_entry, RT_NULL,
                   tele_stack, sizeof(tele_stack), 9, 10);

    rt_thread_startup(&can_rx_thread);
    rt_thread_startup(&ctrl_thread);
    rt_thread_startup(&tele_thread);

    bms_log("[BMS] app started (3 threads)\r\n");
}
