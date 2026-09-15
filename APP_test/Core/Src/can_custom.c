/* ==========================================================================
 * can_custom.c —— CAN 双总线收发 + 分发
 *
 * 两条总线（均为回环自测，无真实收发器）：
 *   CAN1：分流器（0x52x）、整车 DRIVE_ENABLE(0x188)
 *   CAN2：发送 BMS 状态帧(0x301-0x303)、充电请求；接收充电机 CCS 扩展帧
 *        （以 CCS 作为"整车请求充电"的判据，据此置 charge_mode）
 *
 * 收发模型：**ISR 只入队，线程出队分发**。中断回调 ftcan_msg_received_cb() 把帧写进
 * 环形缓冲 can_ring（单生产者：CAN1_RX0 与 CAN2_RX0 同优先级 5，互不抢占，合力等效
 * 单生产者；消费者只有 bms_can_rx 线程），然后释放信号量唤醒线程。载荷与读写索引都
 * volatile，配合 __DMB() 保证"载荷先就绪、索引后发布"。
 * ========================================================================== */
#include "can_custom.h"
#include "bms_io.h"
#include "can.h"
#include "main.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "soc_estimation.h"
#include "state_machine.h"
#include "telemetry_serial.h"
#include "can-halal.h"

#include <rtthread.h>
#include <stdint.h>

static ftcan_t bms_can1;
static ftcan_t bms_can2;

/* charge_mode：整车请求充电（收到充电机 CCS 帧置 1，charge_cmd_check 超时清 0）。
 * drive_mode：行车模式（DRIVE_ENABLE 报文 bit2），有独立心跳看门狗。
 * charge_cmd_timestamp：charge_mode 置 1 的时刻，用于 1s 过期。 */
uint8_t charge_mode = 0;
volatile uint8_t drive_mode = 0;
static uint32_t charge_cmd_timestamp = 0;

/* ---- CAN RX 环形缓冲 ----
 * 中断回调(HAL_CAN_RxFifo0MsgPendingCallback 上下文)只做入队，不调用
 * shunt/charge/sm 处理函数；由 bms_can_rx 线程出队并分发。单生产者单消费者。
 *
 * 前提：CAN1_RX0 与 CAN2_RX0 必须保持同一 NVIC 优先级(5)，两者互不抢占，
 * 多来源入队才等效单生产者。改优先级即破坏该前提。
 * 载荷与索引都 volatile，写入/读取处配 __DMB() 保证"载荷先就绪、索引后发布"。 */
static volatile bms_can_msg_t can_ring[BMS_CAN_RING_SIZE];
static volatile uint16_t can_ring_w = 0;
static volatile uint16_t can_ring_r = 0;
static volatile uint32_t can_ring_drop = 0;   /* 环形缓冲满导致的丢帧数 */

/* bms_app.c 提供：唤醒 CAN RX 线程 */
extern void bms_can_rx_notify(void);

/* 初始化两条 CAN 总线并装过滤/启动。任一失败即返回（调用方不检查，总线会是 Stop）。
 * 过滤：CAN1 用掩码 0x7F0 收 0x52x 段；DRIVE_ENABLE 用全匹配（0x700 过滤保留但
 * 不再处理）；CAN2 收 CCS 扩展帧。 */
HAL_StatusTypeDef can_init(void) {
    HAL_StatusTypeDef status;

    status = ftcan_context_init(&bms_can1, &hcan1, FTCAN_BUS_1, 0,
                                FTCAN_CAN2_START_FILTER_BANK);
    if(status != HAL_OK) return status;

    status = ftcan_context_init(&bms_can2, &hcan2, FTCAN_BUS_2,
                                FTCAN_CAN2_START_FILTER_BANK,
                                FTCAN_FILTER_BANK_COUNT);
    if(status != HAL_OK) return status;

    status = ftcan_add_filter(&bms_can1, CAN_ID_SHUNT_BASE, 0x7F0);
    if(status != HAL_OK) return status;

    status = ftcan_add_filter(&bms_can1, CAN_ID_BMS_IN, 0x7FF);
    if(status != HAL_OK) return status;

    status = ftcan_add_filter(&bms_can1, CAN_ID_DRIVE_ENABLE, 0x7FF);
    if(status != HAL_OK) return status;

    status = ftcan_add_ext_filter(&bms_can2, CAN_ID_CHARGER_CCS,
                                  0x1FFFFFFF);
    if(status != HAL_OK) return status;

    status = ftcan_start(&bms_can1);
    if(status != HAL_OK) return status;

    status = ftcan_start(&bms_can2);
    if(status != HAL_OK) return status;

    return HAL_OK;
}

/* 周期发送三帧 BMS 状态到 CAN2。三帧之间插 2ms 延时避开总线拥塞/硬件 TX 信箱。 */
HAL_StatusTypeDef can_send_status() {
    uint8_t data1[8];
    uint8_t data2[8];
    uint8_t data3[4];
    HAL_StatusTypeDef status;

    /* 在锁内一次性取一致快照(ts_state/shunt/电芯min-max/Bat_Volt/AIR回读)，
     * 拼完帧立刻解锁——发送是阻塞的，不能持锁 */
    bms_state_lock();

    /* STATUS1: 状态/目标态/SDC 打包 + min/max 电压 + max 温度 */
    data1[0] = ts_state.current_state | (ts_state.target_state << 4) | (sdc_closed << 7);
    data1[1] = (uint8_t)(current_soc + 0.5f);   /* SOC %，已钳位到 [0,100] */
    ftcan_marshal_unsigned(&data1[2], min_voltage, 2);
    ftcan_marshal_unsigned(&data1[4], max_voltage, 2);
    ftcan_marshal_signed(&data1[6], max_temp, 2);

    /* STATUS2: 电池总压 + AIR/POWER_UP 实际电平位 + 从机/分流错误标志 + 细分错误位图 */
    ftcan_marshal_signed(&data2[0], Bat_Volt, 4);
    data2[4] = (((bms_in(POS_AIR_CTRL_GPIO_Port, POS_AIR_CTRL_Pin) == GPIO_PIN_SET) ? 1 : 0) << 7)
               | (((bms_in(NEG_AIR_CTRL_GPIO_Port, NEG_AIR_CTRL_Pin) == GPIO_PIN_SET) ? 1 : 0) << 6)
               | (((bms_in(PRECHARGE_CTRL_GPIO_Port, PRECHARGE_CTRL_Pin) == GPIO_PIN_SET) ? 1 : 0) << 5)
               | (((bms_in(POWER_UP_GPIO_Port, POWER_UP_Pin) == GPIO_PIN_SET) ? 1 : 0) << 4);

    data2[5] = (slave_error << 7) | (shunt_error << 6);
    ftcan_marshal_unsigned(&data2[6], ts_error_kind, 2);

    /* STATUS3: 分流器电流 */
    ftcan_marshal_signed(&data3[0], shunt_data.current, 4);

    bms_state_unlock();

    status = ftcan_transmit(&bms_can2, CAN_ID_BMS_STATUS1, data1, sizeof(data1));
    if(status != HAL_OK) return status;
    rt_thread_mdelay(2);

    status = ftcan_transmit(&bms_can2, CAN_ID_BMS_STATUS2, data2, sizeof(data2));
    if(status != HAL_OK) return status;
    rt_thread_mdelay(2);

    return ftcan_transmit(&bms_can2, CAN_ID_BMS_STATUS3, data3, sizeof(data3));
}

/*
HAL_StatusTypeDef can_send_error(TSErrorKind kind, uint8_t device, int16_t value) {
    uint8_t data[4];
    data[0] = kind;
    data[1] = device;
    ftcan_marshal_signed(&data[2], value, 2);
    return ftcan_transmit(&bms_can1, CAN_ID_BMS_ERROR, data, sizeof(data));
}
*/

/*
HAL_StatusTypeDef can_send_error_state() {
    uint8_t data[2];
    ftcan_marshal_unsigned(&data[0], ts_error_kind, 2);
    return ftcan_transmit(&bms_can1, CAN_ID_BMS_ERROR, data, sizeof(data));
}
*/

/* 发充电请求到充电机：电压/电流上限 + 使能位（POWER_UP 未拉高时置 1 请求）。
 * 对齐新版 BMS_SOFTWARE：使能判据取 POWER_UP，而非 CHARGE_ENABLE。 */
HAL_StatusTypeDef can_send_charge_req() {
    uint8_t data[8] = {0};
    ftcan_marshal_unsigned(&data[0], MAX_CHARGE_VOLT, 2);
    ftcan_marshal_unsigned(&data[2], MAX_CHARGE_CURR, 2);
    data[4] = bms_in(POWER_UP_GPIO_Port, POWER_UP_Pin) == GPIO_PIN_SET ? 0 : 1;
    return ftcan_transmit(&bms_can2, CAN_ID_CHARGE_REQ, data, sizeof(data));
}

/* ISR 上下文：仅入队 + 唤醒线程，不做消息处理。
 * 满则丢帧并累加计数。写载荷→__DMB()→推进写索引→释放信号量。 */
void ftcan_msg_received_cb(ftcan_t *ctx, uint32_t id, size_t datalen,
                           const uint8_t *data) {
    /* ISR 上下文：仅入队 + 唤醒线程，不做消息处理 */
    uint16_t next = (uint16_t)((can_ring_w + 1U) % BMS_CAN_RING_SIZE);
    volatile bms_can_msg_t *slot;
    uint8_t n;
    uint8_t i;

    if (next == can_ring_r) {   /* 满 */
        can_ring_drop++;
        return;
    }

    slot = &can_ring[can_ring_w];
    slot->bus = (ctx == &bms_can1) ? 1U : 2U;
    slot->id = id;
    n = (datalen > 8U) ? 8U : (uint8_t)datalen;
    slot->dlc = n;
    for (i = 0U; i < n; i++) {
        slot->data[i] = data[i];
    }
    __DMB();                    /* 载荷写完，再发布可读索引 */
    can_ring_w = next;

    bms_can_rx_notify();
}

/* 线程上下文出队一帧。先 __DMB() 读载荷，再推进读索引；空返回 0。 */
int can_custom_msg_pop(bms_can_msg_t *out) {
    if (can_ring_r == can_ring_w) {
        return 0;
    }
    __DMB();                    /* 先读载荷，再推进出队索引 */
    *out = can_ring[can_ring_r];
    can_ring_r = (uint16_t)((can_ring_r + 1U) % BMS_CAN_RING_SIZE);
    return 1;
}

uint32_t can_custom_drop_count(void) {
    return can_ring_drop;
}

/* 按总线+ID 分发。分流器帧走掩码匹配（0x52x 段），其余精确匹配。
 * CCS/DRIVE_ENABLE 的写入此处持锁，避免与控制线程读 charge_mode/drive_mode 竞争。
 * 新版把"整车请求充电"的判据从 0x700(BMS_IN) 改为充电机 CCS 帧，故 CCS 分支兼做
 * 置 charge_mode 的副作用（charge_handle_can_message 仍为空实现）。 */
void can_custom_dispatch(const bms_can_msg_t *m) {
    if(m->bus == 1U && (m->id & 0x7F0U) == CAN_ID_SHUNT_BASE) {
        shunt_handle_can_message((uint16_t)m->id, m->data);
        return;
    }
    else if(m->bus == 2U && m->id == CAN_ID_CHARGER_CCS) {
        charge_handle_can_message(m->data);
        /* CAN RX 线程写，控制线程读——需互斥 */
        bms_state_lock();
        charge_mode = 1;
        charge_cmd_timestamp = HAL_GetTick();
        bms_state_unlock();
        return;
    }
    else if(m->bus == 1U && m->id == CAN_ID_DRIVE_ENABLE) {
        if(m->dlc < 2U) {
            drive_mode = 0;
            return;
        }
        sm_handle_can_message(m->data);
        return;
    }
}

/* ---- 回环自测：把合成帧发到回环总线，再经 RX 过滤/分发回来 ----
 * 数值与 LTC68041_stub 对齐：10 个 IC × 115 芯 × 3700 mV → Bat_Volt = 425500 mV，
 * 所以分流器 battery 电压（0x524）也发 425500，电压不一致(阈值 10000)不会误报；
 * 车辆侧电压(0x523)同值，让预充条件(>0.95*电池电压)成立，状态机可走到 ACTIVE。 */
static int can_selftest_on = 0;
static uint32_t can_selftest_last = 0;

/* 开关回环自测；置 0 时下次 tick 立即重发。 */
void can_custom_selftest(int enable) {
    can_selftest_on = enable ? 1 : 0;
    can_selftest_last = 0;
    bms_log("[CAN] selftest %s\r\n", can_selftest_on ? "ON" : "OFF");
}

/* 每 200ms 往 CAN1/CAN2 灌一组"合法"的合成帧，用来端到端验证 RX→过滤→分发。
 * 每 5 拍（1s）打一行诊断（tx 状态、环读写指针、丢帧数）。 */
void can_custom_selftest_tick(void) {
    uint8_t d[6];
    static uint8_t diag_div = 0;
    HAL_StatusTypeDef tx1;

    if (!can_selftest_on) {
        return;
    }
    if (HAL_GetTick() - can_selftest_last < 200U) {
        return;
    }
    can_selftest_last = HAL_GetTick();

    /* CAN1: 电流 0x521（6 字节，低于超流阈值） */
    ftcan_marshal_signed(&d[0], 2000, 6);
    tx1 = ftcan_transmit(&bms_can1, CAN_ID_SHUNT_CURRENT, d, 6);

    /* CAN1: 电池总压 0x524（U3，新版起电压_bat 取此通道） */
    ftcan_marshal_signed(&d[0], 425500, 6);
    ftcan_transmit(&bms_can1, CAN_ID_SHUNT_VOLTAGE_3, d, 6);

    /* CAN1: 车辆侧电压 0x523（沿用上面的 425500） */
    ftcan_transmit(&bms_can1, CAN_ID_SHUNT_VOLTAGE_2, d, 6);

    /* CAN1: 母线温度 0x525（低于过温阈值 1000） */
    ftcan_marshal_signed(&d[0], 250, 6);
    ftcan_transmit(&bms_can1, CAN_ID_SHUNT_BUSBAR_TEMP, d, 6);

    /* CAN2: 充电机 CCS（扩展帧） */
    ftcan_marshal_unsigned(&d[0], 5000, 2);
    ftcan_marshal_unsigned(&d[2], 10, 2);
    ftcan_transmit_ext(&bms_can2, CAN_ID_CHARGER_CCS, d, 6);

    diag_div++;
    if (diag_div >= 5U) {
        diag_div = 0;
        if (!telemetry_host_active()) {
            bms_log("[CAN] tx=%u st=%u ring w=%u r=%u drop=%lu last=%lu\r\n",
                    (unsigned)tx1,
                    (unsigned)hcan1.State,
                    (unsigned)can_ring_w,
                    (unsigned)can_ring_r,
                    (unsigned long)can_ring_drop,
                    (unsigned long)shunt_data.last_message);
        }
    }
}

/* charge_mode 过期检查：置 1 后超过 CHARGE_CMD_TIMEOUT(1s) 无新帧即自动清 0。
 * 由驱动层在取状态机快照前调用（且仅 GPIO 模式且 POWER_UP 时），见 state_machine.c。 */
void charge_cmd_check() {
    bms_state_lock();
    if(charge_mode == 1) {
        if(HAL_GetTick() - charge_cmd_timestamp > CHARGE_CMD_TIMEOUT) {
            charge_mode = 0;
            charge_cmd_timestamp = 0;
        }
    }
    else {
        charge_mode = 0;
    }
    bms_state_unlock();
}

/* 故障注入入口：直接置/清 charge_mode 并刷时间戳（模拟收到充电机 CCS 帧）。 */
int charge_inject(int32_t mode) {
    bms_state_lock();
    charge_mode = mode ? 1U : 0U;
    charge_cmd_timestamp = HAL_GetTick();
    bms_state_unlock();
    return 0;
}
