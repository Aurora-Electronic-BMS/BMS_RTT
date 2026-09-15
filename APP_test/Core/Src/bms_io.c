/* bms_io.c —— BMS 数字 IO 的虚拟层（逻辑验证用）
 *
 * 目的：在没有真实 BMS 硬件的条件下验证状态机/保护逻辑。
 *   - 输出（BMS_NERROR / HV_ACTIVE / POS_AIR / NEG_AIR / PRECHARGE / BMS_ERROR /
 *     HV_MISMATCH / STATUS_x）：只在电平变化时通过串口打印一行，不驱动真实引脚。
 *   - 输入（POWER_UP / CHARGE_ENABLE / SDC_VOLTAGE）：值由串口命令注入，
 *     bms_in() 返回注入值而非引脚电平（引脚未接线读回来是随机的，无意义）。
 *   - 输出被代码回读时（POS/NEG/PRECHARGE 自检、HV_ACTIVE 上报），返回最后写入值。
 *
 * 线程安全：sig_tab 的 level、sdc_closed 等会被多个线程读写，统一用 state_mtx（递归锁）
 * 保护；串口打印 tx_mtx 保护。铁律：绝不跨阻塞调用持锁（HAL_UART_Transmit 在锁外）。
 */
#include "bms_io.h"
#include "main.h"
#include <rtthread.h>
#include <rthw.h>
#include <stdarg.h>

extern void rt_hw_console_output(const char *str);

/* state_machine.c / telemetry_serial.c 直接引用此变量 */
int sdc_closed = 1;

/* 上位机会话活跃标志：telemetry_serial.c 维护，这里只读。
 * 活跃时 bms_log 静默，避免文本行插进 B5 4D 二进制流。 */
volatile int bms_host_active = 0;

/* 多线程安全日志：rt_kprintf 内部共用一个格式化缓冲，多线程并发会把彼此
 * 的字符串冲散（实测出现 "app sta"+"read up" 这种半行交错）。这里改成先
 * 格式化到调用者自己的栈缓冲，再交给串口输出（输出端有 TX 互斥）。
 * force=1 时即便上位机在线也照打（用于 [DUMP] 这类主动应答）。 */
static void bms_log_v(int force, const char *fmt, va_list args) {
    char buf[192];

    if (!force && bms_host_active) {
        return;                 /* 上位机会话中：保持二进制流干净 */
    }
    rt_vsnprintf(buf, sizeof(buf), fmt, args);
    rt_hw_console_output(buf);
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
} bms_sig_t;

/* 信号表。port/pin 与真实原理图对应，但本层只记录 level 并打印，不碰寄存器。
 * SDC_VOLTAGE 初值 1（默认视为闭合），其余 0。 */
static bms_sig_t sig_tab[] = {
    /* ---- 输出 ---- */
    { GPIOB, GPIO_PIN_0,  "BMS_NERROR",   0U, SIG_ID_NONE,          0U },
    { GPIOE, GPIO_PIN_2,  "BMS_ERROR",    0U, SIG_ID_NONE,          0U },
    { GPIOE, GPIO_PIN_8,  "ERROR_LED",    0U, SIG_ID_NONE,          0U },
    { GPIOE, GPIO_PIN_10, "HV_ACTIVE",    0U, SIG_ID_NONE,          0U },
    { GPIOB, GPIO_PIN_15, "PRECHARGE",    0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_9,  "NEG_AIR",      0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_11, "POS_AIR",      0U, SIG_ID_NONE,          0U },
    { GPIOC, GPIO_PIN_4,  "HV_MISMATCH",  0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_15, "STATUS_4",     0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_13, "STATUS_6",     0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_14, "STATUS_5",     0U, SIG_ID_NONE,          0U },
    { GPIOC, GPIO_PIN_8,  "STATUS_1",     0U, SIG_ID_NONE,          0U },
    { GPIOC, GPIO_PIN_6,  "STATUS_3",     0U, SIG_ID_NONE,          0U },
    { GPIOC, GPIO_PIN_7,  "STATUS_2",     0U, SIG_ID_NONE,          0U },
    { GPIOD, GPIO_PIN_0,  "SPI3_CS",      0U, SIG_ID_NONE,          0U },
    { GPIOB, GPIO_PIN_6,  "SPI1_CS",      0U, SIG_ID_NONE,          0U },
    /* ---- 输入（可注入）---- */
    { GPIOE, GPIO_PIN_5,  "POWER_UP",      1U, BMS_SIG_POWER_UP,      0U },
    { GPIOE, GPIO_PIN_4,  "CHARGE_ENABLE", 1U, BMS_SIG_CHARGE_ENABLE, 0U },
    { GPIOE, GPIO_PIN_3,  "SDC_VOLTAGE",   1U, BMS_SIG_SDC_VOLTAGE,   1U },
};

#define SIG_COUNT (sizeof(sig_tab) / sizeof(sig_tab[0]))

static struct rt_mutex tx_mtx;
static rt_uint8_t tx_mtx_ready = 0U;

/* 保护共享 BMS 状态。递归锁：同一线程可重复 take（hold++），owner 才可 release。 */
static struct rt_mutex state_mtx;
static rt_uint8_t state_mtx_ready = 0U;

/* *锁在 bms_io_init 里才创建；_ready 标志让 INIT_APP_EXPORT 之前的早期调用安全空转。 */
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

/* 按 (port,pin) 查信号表；未映射返回 RT_NULL。 */
static bms_sig_t *sig_find(GPIO_TypeDef *port, uint16_t pin)
{
    rt_size_t i;
    for (i = 0U; i < SIG_COUNT; i++) {
        if (sig_tab[i].port == port && sig_tab[i].pin == pin) {
            return &sig_tab[i];
        }
    }
    return RT_NULL;
}

/* 统一的输出打印格式：[OUT] NAME=level / [INJ] NAME=level */
static void io_emit(const char *tag, const char *name, uint8_t level)
{
    bms_log("[%s] %s=%d\r\n", tag, name, level);
}

/* 虚拟"写引脚"：更新表中电平，仅在**变化**时打印一行（去重刷屏）。
 * 未映射的引脚也打印，便于发现遗漏的映射。
 * 日志走 UART 是阻塞调用——先把状态改完、解锁，再打印。 */
void bms_out(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    bms_sig_t *s;
    const char *changed = RT_NULL;
    uint8_t v = (state == GPIO_PIN_SET) ? 1U : 0U;

    /* 日志走 UART 是阻塞调用——先把状态改完、解锁，再打印 */
    bms_state_lock();
    s = sig_find(port, pin);
    if (s == RT_NULL) {
        bms_state_unlock();
        bms_log("[OUT] unmapped pin=%u value=%d\r\n", (unsigned)pin, v);
        return;
    }
    if (s->level != v) {
        s->level = v;
        changed = s->name;
    }
    bms_state_unlock();

    if (changed != RT_NULL) {
        io_emit("OUT", changed, v);
    }
}

/* 虚拟"读引脚"：返回表中记录的电平（输入=注入值，输出=最后写入值）。
 * 未映射一律返回 RESET（0），与"悬空读 0"的行为一致。 */
GPIO_PinState bms_in(GPIO_TypeDef *port, uint16_t pin)
{
    bms_sig_t *s;
    uint8_t level;
    bms_state_lock();
    s = sig_find(port, pin);
    level = (s == RT_NULL) ? 0U : s->level;
    bms_state_unlock();
    return level ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

/* 注入数字量输入。SDC_VOLTAGE 特殊：它直接改全局 sdc_closed（状态机的输入之一），
 * 不走 sig_tab 的 level。其余可注入输入按 id 查表后改 level。
 * 返回 0 成功，-1 未知信号。变化才打印（与 bms_out 一致）。 */
int bms_io_inject(uint8_t sig, uint8_t value)
{
    bms_sig_t *s;
    const char *changed = RT_NULL;
    uint8_t v = value ? 1U : 0U;
    int ret = -1;

    bms_state_lock();
    if (sig == BMS_SIG_SDC_VOLTAGE) {
        sdc_closed = v ? 1 : 0;
        bms_state_unlock();
        io_emit("INJ", "SDC_VOLTAGE", v);
        return 0;
    }

    for (s = sig_tab; s < sig_tab + SIG_COUNT; s++) {
        if (s->is_input && s->id == sig) {
            if (s->level != v) {
                s->level = v;
                changed = s->name;
            }
            ret = 0;
            break;
        }
    }
    bms_state_unlock();

    if (changed != RT_NULL) {
        io_emit("INJ", changed, v);
    }
    return ret;
}

/* 上位机请求的 IO 全量快照：逐行打印每个信号的名字/方向/电平。
 * level 需锁内取值（会被别的线程改），name/is_input 初始化后不变可无锁读。 */
void bms_io_dump(void)
{
    uint8_t level[SIG_COUNT];
    rt_size_t i;

    /* name/is_input 初始化后不变，可无锁读；level 需在锁内取快照 */
    bms_state_lock();
    for (i = 0U; i < SIG_COUNT; i++) {
        level[i] = sig_tab[i].level;
    }
    bms_state_unlock();

    for (i = 0U; i < SIG_COUNT; i++) {
        /* 上位机主动请求的应答，会话中也要输出 */
        bms_log_force("[DUMP] %s %s=%d\r\n", sig_tab[i].name,
                      sig_tab[i].is_input ? "in" : "out", level[i]);
    }
}
