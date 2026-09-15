/* bms_app.c —— 把 BMS 应用层接到 RT-Thread 上
 *
 * 原 BMS_V1.0 工程是裸机 while(1) 轮询。这里保持业务模块(state_machine /
 * shunt_monitor / slave_monitor / can_custom / telemetry_serial)逻辑不变，
 * 只把它们拆成三个 RT-Thread 线程：
 *   bms_can_rx    信号量驱动：排空 CAN 环形缓冲并分发(shunt/charge/drive)
 *   bms_ctrl      50ms 固定周期：状态机每拍跑；电芯/分流/上报 150ms 分频
 *   bms_telemetry 事件驱动：串口 RX 中断唤醒收帧，超时兜底 200ms 周期上报
 *
 * 线程在 main() 的外设初始化之后、由 bms_app_start() 创建并启动，
 * 因此不需要"入口自旋等 main 初始化完"的 bms_ready 标志。
 *
 * 线程优先级（数值越小越高）：can_rx=7 > ctrl=8 > tele=9，三者都比 main 线程高。
 * 这样 CAN/串口事件能被及时消费，而状态机与控制流仍按固定节奏推进。
 */
#include <rtthread.h>
#include "bms_io.h"
#include "can.h"
#include "can_custom.h"
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

/* 由 can_custom.c 的 ISR 回调调用：每入队一帧 CAN 消息就释放一次信号量。
 * ISR 里只做 rt_sem_release（RT-Thread 保证其 ISR 安全）。 */
void bms_can_rx_notify(void) {
    rt_sem_release(&can_rx_sem);
}

/* CAN RX 线程主体：等信号量 → 排空环形缓冲 → 逐帧分发。
 * 用 while 排空（而非每信号量一帧）是为了在消息风暴时一次清空积压，
 * 减少切换开销；分发本身在**线程上下文**做，ISR 只负责入队。 */
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
 * remain>0：睡到该点，并把下次锚点顺推一个周期（保持节奏不漂移）；
 * 否则：说明已经错过锚点，直接以"现在+周期"重锚，丢弃漏掉的拍。 */
static void bms_delay_until(rt_tick_t *next, rt_tick_t period)
{
    rt_int32_t remain = (rt_int32_t)(*next - rt_tick_get());

    if (remain > 0) {
        rt_thread_delay((rt_tick_t)remain);
        *next += period;
    }
    else {
        *next = rt_tick_get() + period;
    }
}

/* 控制线程主体：50ms 一拍。"重活"（电芯采样+分流检查+CAN 状态帧+回环自测）
 * 每 3 拍做一次（150ms），轻活（状态机推进）每拍都做。
 * last_state/last_status 用于"变化才打印"，避免刷屏。 */
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

        if(++heavy_div >= BMS_HEAVY_DIV) {
            heavy_div = 0U;
            heavy = 1U;
        }

        /* 保持原裸机相对顺序：先采电芯/分流，再跑状态机，最后上报 */
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

        /* LED1 = BMS 已启动且状态机非 INACTIVE；LED2 = 有故障 */
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin,
                          (ts_state.current_state == TS_INACTIVE) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin,
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
        /* 事件驱动：串口来了字节立即处理；否则睡到下次 200ms 上报点 */
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

/* 板载没有 CAN 收发器时 CAN_RX 悬空，bxCAN 会一直卡在初始化模式(MSR.INAK=1)
 * 导致 HAL_CAN_Start 超时。打开内部上拉把总线拉到空闲(recessive)。 */
static void can_rx_pullup(void) {
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Pin = GPIO_PIN_11;
    gpio.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_12;
    gpio.Alternate = GPIO_AF9_CAN2;
    HAL_GPIO_Init(GPIOB, &gpio);
}

/* 由 main() 在外设 MX_*_Init 之后调用：初始化各业务模块，再创建并启动三个线程。
 * 顺序有讲究，见各处行内注释。 */
void bms_app_start(void) {
    can_rx_pullup();

    /* 信号量必须先于 can_init：CAN 中断可能在使能后立刻回调 */
    rt_sem_init(&can_rx_sem, "canrx", 0, RT_IPC_FLAG_FIFO);

    can_init();
    slaves_init();
    shunt_init();
    soc_init();
    ts_sm_init();

    /* 收窄 CAN/UART 中断优先级(覆盖 CubeMX 生成的 0)：避免消息风暴时
     * 长时间压住 SysTick(最低优先级)导致 RT-Thread 调度停摆 */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5, 0);
    HAL_NVIC_SetPriority(CAN2_RX0_IRQn, 5, 0);
    HAL_NVIC_SetPriority(UART5_IRQn, 5, 0);
    HAL_NVIC_SetPriority(USART6_IRQn, 5, 0);

    /* 两个串口都能跑 B5 4D 协议(含 0x13 重入升级) */
    telemetry_serial_init(&huart5, &huart6);

    /* 外设已就绪，此时才创建并启动线程。
     * 优先级：can_rx(7) 最高以保证入队及时；ctrl(8) 次之；tele(9) 最低。
     * 后两参 10 为时间片(ticks)，同优先级轮转时才用得上。 */
    rt_thread_init(&can_rx_thread, "bms_can_rx", bms_can_rx_entry, RT_NULL,
                   can_rx_stack, sizeof(can_rx_stack), 7, 10);
    rt_thread_init(&ctrl_thread, "bms_ctrl", bms_ctrl_entry, RT_NULL,
                   ctrl_stack, sizeof(ctrl_stack), 8, 10);
    rt_thread_init(&tele_thread, "bms_tele", bms_telemetry_entry, RT_NULL,
                   tele_stack, sizeof(tele_stack), 9, 10);

    rt_thread_startup(&can_rx_thread);
    rt_thread_startup(&ctrl_thread);
    rt_thread_startup(&tele_thread);

    bms_log("[BMS] app started (CAN loopback, virtual IO)\r\n");
}
