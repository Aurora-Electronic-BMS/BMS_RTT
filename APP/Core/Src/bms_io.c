/* bms_io.c —— BMS 数字 IO 的虚拟层（真实引脚 + 故障注入）
 *
 * 目的：在逻辑验证 / 故障注入时统一管理数字 IO。
 *   - 输出（BMS_NERROR / HV_ACTIVE / POS_AIR / NEG_AIR / PRECHARGE / BMS_ERROR /
 *     HV_MISMATCH / STATUS_x）：**正常驱动真实引脚**，并在电平变化时通过串口打印一行。
 *   - 输入（POWER_UP / CHARGE_ENABLE / SDC_VOLTAGE）：默认读真实引脚电平；被上位机
 *     串口注入过之后固定返回注入值（注入优先，供故障注入测试使用）。
 *   - 输出被代码回读时（POS/NEG/PRECHARGE 自检、HV_ACTIVE 上报），读回实际引脚电平。
 *
 * 平台切换（单份源码）：
 *   BMS_USE_RTTHREAD 定义 → 两把锁为 rt_mutex（递归），格式化 rt_vsnprintf，
 *     文本出口 rt_hw_console_output（由 rt_app.c 实现，内部再取 bms_tx_lock）。
 *   未定义（裸机） → 锁为空实现（主循环单线程；RX 中断只入队不碰状态），
 *     格式化 vsnprintf，文本直接 HAL_UART_Transmit 到 USART2。
 * 铁律：绝不跨阻塞调用持锁（HAL_UART_Transmit 在锁外）。
 */
#include "bms_io.h"
#include "main.h"
#include <stdarg.h>
#include <stdio.h>

#ifdef BMS_USE_RTTHREAD
#include <rthw.h>
extern void rt_hw_console_output(const char *str);
#else
#include "usart.h"
#endif

/* state_machine.c 直接引用此变量 */
int sdc_closed = 1;

/* 上位机会话活跃标志：telemetry_serial.c 维护，这里只读。
 * 活跃时 bms_log 静默，避免文本行插进 B5 4D 二进制流。 */
volatile int bms_host_active = 0;

/* 日志：先格式化到调用者自己的栈缓冲，再交给串口输出（输出端有 TX 互斥）。
 * force=1 时即便上位机在线也照打（用于 [DUMP] 这类主动应答）。 */
static void bms_log_v(int force, const char *fmt, va_list args) {
    char buf[192];

    if (!force && bms_host_active) {
        return;                 /* 上位机会话中：保持二进制流干净 */
    }

#ifdef BMS_USE_RTTHREAD
    rt_vsnprintf(buf, sizeof(buf), fmt, args);
    rt_hw_console_output(buf);
#else
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;   /* vsnprintf 返回"本应写入的长度"，需自行截断 */
    }
    /* 串口还没初始化（Instance 为 NULL）时丢弃早期打印 */
    if (huart1.Instance == NULL) {
        return;
    }
    bms_tx_lock();
    HAL_UART_Transmit(&huart1, (const uint8_t *)buf, (uint16_t)n, 200U);
    bms_tx_unlock();
#endif
}

void bms_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bms_log_v(0, fmt, args);
    va_end(args);
}

void bms_log_force(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bms_log_v(1, fmt, args);
    va_end(args);
}

#define SIG_ID_NONE 0xFFU

/* 一条虚拟信号：把 (port,pin) 映射到名字/方向/当前电平。
 * id 仅对输入有效（用于注入查找），输出恒为 SIG_ID_NONE。 */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *name;
    uint8_t       is_input;
    uint8_t       id;       /* 输入信号的可注入 id，SIG_ID_NONE 表示非输入 */
    uint8_t       level;
    uint8_t       injected; /* 输入是否被上位机注入过：注入后固定用注入值，否则回退读真实引脚 */
} bms_sig_t;

/* 信号表：port/pin 直接取本工程 main.h 的 STM32F407VGT6 映射（与 BMS_V1.0 一致）。
 * 用宏而非硬编码数字：main.h 一旦调整引脚本表自动跟随，避免再次出现"表与实际引脚
 * 错位 → 注入落到别的信号上"。bms_in/bms_out 都按 (port,pin) 查表，故宏必须与调用方
 * 传的宏同源。本层只记录 level 并打印，不碰寄存器。
 * SDC_VOLTAGE 初值 1（默认视为闭合），其余 0。 */
static bms_sig_t sig_tab[] = {
    /* ---- 输出 ---- */
    { BMS_NERROR_GPIO_Port,     BMS_NERROR_Pin,     "BMS_NERROR",   0U, SIG_ID_NONE,          0U },
    { BMS_ERROR_GPIO_Port,      BMS_ERROR_Pin,      "BMS_ERROR",    0U, SIG_ID_NONE,          0U },
    { ERROR_LED_GPIO_Port,      ERROR_LED_Pin,      "ERROR_LED",    0U, SIG_ID_NONE,          0U },
    { HV_ACTIVE_GPIO_Port,      HV_ACTIVE_Pin,      "HV_ACTIVE",    0U, SIG_ID_NONE,          0U },
    { PRECHARGE_CTRL_GPIO_Port, PRECHARGE_CTRL_Pin, "PRECHARGE",    0U, SIG_ID_NONE,          0U },
    { NEG_AIR_CTRL_GPIO_Port,   NEG_AIR_CTRL_Pin,   "NEG_AIR",      0U, SIG_ID_NONE,          0U },
    { POS_AIR_CTRL_GPIO_Port,   POS_AIR_CTRL_Pin,   "POS_AIR",      0U, SIG_ID_NONE,          0U },
    { HV_MISMATCH_GPIO_Port,    HV_MISMATCH_Pin,    "HV_MISMATCH",  0U, SIG_ID_NONE,          0U },
    { STATUS_1_GPIO_Port,       STATUS_1_Pin,       "STATUS_1",     0U, SIG_ID_NONE,          0U },
    { STATUS_2_GPIO_Port,       STATUS_2_Pin,       "STATUS_2",     0U, SIG_ID_NONE,          0U },
    { STATUS_3_GPIO_Port,       STATUS_3_Pin,       "STATUS_3",     0U, SIG_ID_NONE,          0U },
    { STATUS_4_GPIO_Port,       STATUS_4_Pin,       "STATUS_4",     0U, SIG_ID_NONE,          0U },
    { STATUS_5_GPIO_Port,       STATUS_5_Pin,       "STATUS_5",     0U, SIG_ID_NONE,          0U },
    { STATUS_6_GPIO_Port,       STATUS_6_Pin,       "STATUS_6",     0U, SIG_ID_NONE,          0U },
    { SPI1_CS_GPIO_Port,        SPI1_CS_Pin,        "SPI1_CS",      0U, SIG_ID_NONE,          0U },
    { SPI3_CS_GPIO_Port,        SPI3_CS_Pin,        "SPI3_CS",      0U, SIG_ID_NONE,          0U },
    /* ---- 输入（可注入）---- */
    { POWER_UP_GPIO_Port,      POWER_UP_Pin,      "POWER_UP",      1U, BMS_SIG_POWER_UP,      0U },
    { CHARGE_ENABLE_GPIO_Port, CHARGE_ENABLE_Pin, "CHARGE_ENABLE", 1U, BMS_SIG_CHARGE_ENABLE, 0U },
    { SDC_VOLTAGE_GPIO_Port,   SDC_VOLTAGE_Pin,   "SDC_VOLTAGE",   1U, BMS_SIG_SDC_VOLTAGE,   1U },
};

#define SIG_COUNT (sizeof(sig_tab) / sizeof(sig_tab[0]))

#ifdef BMS_USE_RTTHREAD
static struct rt_mutex tx_mtx;
static rt_uint8_t tx_mtx_ready = 0U;

/* 保护共享 BMS 状态。递归锁：同一线程可重复 take（hold++），owner 才可 release。 */
static struct rt_mutex state_mtx;
static rt_uint8_t state_mtx_ready = 0U;

/* 两把锁在 bms_io_init 里才创建；_ready 标志让早期调用安全空转。 */
void bms_tx_lock(void)
{
    if (tx_mtx_ready) rt_mutex_take(&tx_mtx, RT_WAITING_FOREVER);
}

void bms_tx_unlock(void)
{
    if (tx_mtx_ready) rt_mutex_release(&tx_mtx);
}

void bms_state_lock(void)
{
    if (state_mtx_ready) rt_mutex_take(&state_mtx, RT_WAITING_FOREVER);
}

void bms_state_unlock(void)
{
    if (state_mtx_ready) rt_mutex_release(&state_mtx);
}

/* 创建两把互斥锁。在 main() 之前由 INIT_APP_EXPORT 调用，故不碰外设。 */
void bms_io_init(void)
{
    rt_mutex_init(&tx_mtx, "bms_tx", RT_IPC_FLAG_PRIO);
    tx_mtx_ready = 1U;
    rt_mutex_init(&state_mtx, "bms_state", RT_IPC_FLAG_PRIO);
    state_mtx_ready = 1U;
    bms_log("[IO] virtual io ready (outputs=print, inputs=inject)\r\n");
}

#else /* 裸机：主循环单线程，RX 中断只入队，不需要锁 */

void bms_tx_lock(void) { }
void bms_tx_unlock(void) { }
void bms_state_lock(void) { }
void bms_state_unlock(void) { }

void bms_io_init(void)
{
    /* 什么都不用建；留着接口让两棵树共用同一份调用点 */
}

#endif /* BMS_USE_RTTHREAD */

/* 按 (port,pin) 查信号表；未映射返回 NULL。 */
static bms_sig_t *sig_find(GPIO_TypeDef *port, uint16_t pin)
{
    for (size_t i = 0U; i < SIG_COUNT; i++) {
        if (sig_tab[i].port == port && sig_tab[i].pin == pin) {
            return &sig_tab[i];
        }
    }
    return NULL;
}

/* 统一的输出打印格式：[OUT] NAME=level / [INJ] NAME=level */
static void io_emit(const char *tag, const char *name, uint8_t level)
{
    bms_log("[%s] %s=%d\r\n", tag, name, level);
}

/* 虚拟"写引脚"：**真实驱动引脚**（HAL_GPIO_WritePin），并更新表中电平，
 * 仅在**变化**时打印一行（去重刷屏）。未映射的引脚也打印，便于发现遗漏的映射。
 * 日志走 UART 是阻塞调用——先把状态改完、解锁，再打印。 */
void bms_out(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    bms_sig_t *s;
    const char *changed = NULL;
    uint8_t v = (state == GPIO_PIN_SET) ? 1U : 0U;

    HAL_GPIO_WritePin(port, pin, state);    /* 输出信号：正常驱动真实引脚 */

    bms_state_lock();
    s = sig_find(port, pin);
    if (s == NULL) {
        bms_state_unlock();
        bms_log("[OUT] unmapped pin=%u value=%d\r\n", (unsigned)pin, v);
        return;
    }
    if (s->level != v) {
        s->level = v;
        changed = s->name;
    }
    bms_state_unlock();

    if (changed != NULL) {
        io_emit("OUT", changed, v);
    }
}

/* 虚拟"读引脚"：
 *   - 输入信号：被注入过则返回注入值（上位机串口输入优先），否则读回真实引脚；
 *   - 输出信号：**读回实际引脚电平**（bms_out 已真实驱动）；
 *   - 未映射：读真实引脚（等价于悬空读 0）。 */
GPIO_PinState bms_in(GPIO_TypeDef *port, uint16_t pin)
{
    bms_sig_t *s;
    uint8_t level;

    bms_state_lock();
    s = sig_find(port, pin);
    if (s != NULL && s->is_input && s->injected) {
        level = s->level;                                   /* 串口注入优先 */
    }
    else {
        level = (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U;
        if (s != NULL) {
            s->level = level;
        }
    }
    bms_state_unlock();
    return level ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

#if BMS_TEST_HOOKS
/* 注入数字量输入。SDC_VOLTAGE 特殊：它直接改全局 sdc_closed（状态机的输入之一），
 * 同时置 injected 标志使 bms_in 返回注入值。其余可注入输入按 id 查表后改 level。
 * 返回 0 成功，-1 未知信号。变化才打印（与 bms_out 一致）。 */
int bms_io_inject(uint8_t sig, uint8_t value)
{
    bms_sig_t *s;
    const char *changed = NULL;
    uint8_t v = value ? 1U : 0U;
    int ret = -1;

    bms_state_lock();
    for (s = sig_tab; s < sig_tab + SIG_COUNT; s++) {
        if (s->is_input && s->id == sig) {
            if (s->level != v) {
                changed = s->name;
            }
            s->level = v;
            s->injected = 1U;
            ret = 0;
            break;
        }
    }
    if (ret == 0 && sig == BMS_SIG_SDC_VOLTAGE) {
        sdc_closed = v ? 1 : 0;
    }
    bms_state_unlock();

    if (changed != NULL) {
        io_emit("INJ", changed, v);
    }
    return ret;
}

/* 上位机请求的 IO 全量快照：逐行打印每个信号的名字/方向/电平。
 * level 需锁内取值（会被别的线程改），name/is_input 初始化后不变可无锁读。 */
void bms_io_dump(void)
{
    uint8_t level[SIG_COUNT];

    bms_state_lock();
    for (size_t i = 0U; i < SIG_COUNT; i++) {
        level[i] = sig_tab[i].level;
    }
    bms_state_unlock();

    for (size_t i = 0U; i < SIG_COUNT; i++) {
        /* 上位机主动请求的应答，会话中也要输出 */
        bms_log_force("[DUMP] %s %s=%d\r\n", sig_tab[i].name,
                      sig_tab[i].is_input ? "in" : "out", level[i]);
    }
}
#endif
