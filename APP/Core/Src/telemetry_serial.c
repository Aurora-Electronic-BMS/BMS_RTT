/* ==========================================================================
 * telemetry_serial.c —— B5 4D 二进制协议：遥测上报 + 上位机命令
 *
 * 帧格式（小端）：
 *   MAGIC0(0xB5) MAGIC1(0x4D) VER(1) MSG_TYPE(1) SEQ(u16) LEN(u16) DATA[LEN] CRC16(u16)
 *   CRC16-Modbus 覆盖 [VER..DATA]（从 VER 字节起到载荷末）。
 *
 * 两个串口（USART2、USART3）都能跑本协议。链接模型：**同一时刻只认一个"上位机口"**
 *   —— 收到某口的有效帧时，若当前无活跃上位机则切到该口；已有活跃上位机则忽略别口的帧。
 *   MSG_HEARTBEAT 置 bms_host_active=1 并刷新 last_host_heartbeat；超过 LINK_TIMEOUT
 *   无心跳即判链路断开（bms_host_active=0、telemetry_uart=0）。活跃期间 bms_log 静默。
 *
 * 收发模型：RX 逐字节中断 → 环形缓冲（单生产者 ISR、单消费者主循环/遥测线程）→
 * 状态机拼帧 → CRC 校验 → 分发命令。TX 走 bms_tx_lock 与打印互斥。
 *
 * 平台切换（单份源码）：
 *   BMS_USE_RTTHREAD 定义 → RX 信号量驱动，telemetry_serial_wait_next() 睡到"来字节"
 *     或"距下次 200ms 上报点"较早者（由遥测线程调用）。
 *   未定义（裸机） → 无信号量，主循环每拍调 telemetry_serial_process() 排空 RX 环；
 *     不调用 wait_next（该函数不参与裸机编译）。
 * ========================================================================== */
#include "telemetry_serial.h"
#include "bms_io.h"
#include "can_custom.h"
#include "main.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "soc_estimation.h"
#include "state_machine.h"

#include <stdint.h>
#include <string.h>

#ifdef BMS_USE_RTTHREAD
#include <rtthread.h>
#endif

#define TELEMETRY_MAGIC0 0xB5U
#define TELEMETRY_MAGIC1 0x4DU
#define TELEMETRY_VERSION 1U
#define TELEMETRY_PREFIX_LEN 6U            /* VER+TYPE+SEQ(2)+LEN(2) */
#define TELEMETRY_MAX_PAYLOAD 512U
#define TELEMETRY_RX_RING_SIZE 512U
#define TELEMETRY_TX_PERIOD 200U           /* 遥测上报周期 ms */
#define TELEMETRY_LINK_TIMEOUT 1000U       /* 无心跳判链路断超时 ms */
#define TELEMETRY_PORT_COUNT 2U
#define BOOT_REQUEST_MAGIC 0xB007B007UL    /* 写 BKP0R 的"请求进 bootloader"魔数 */

#define MSG_TELEMETRY_FULL 0x01U
#define MSG_ACK 0x02U
#define MSG_SET_TARGET_STATE 0x10U
#define MSG_SET_MANUAL_AIR 0x11U
#define MSG_HEARTBEAT 0x12U
#define MSG_ENTER_BOOTLOADER 0x13U
#define MSG_CLEAR_ERRORS 0x14U
#define MSG_SET_INPUT 0x20U
#define MSG_DUMP_IO 0x21U
#define MSG_CAN_SELFTEST 0x22U
#define MSG_INJECT 0x23U

#define ACK_OK 0U
#define ACK_BAD_LENGTH 2U
#define ACK_UNKNOWN_COMMAND 3U
#define ACK_REJECTED 4U

/* 遥测载荷布局常量。CELL_SLOTS=144、TEMP_SLOTS=48，逐芯/逐温上报。 */
#define CELL_GROUPS 12U
#define CELL_CHANNELS 12U
#define CELL_SLOTS (CELL_GROUPS * CELL_CHANNELS)
#define TEMP_GROUPS 12U
#define TEMP_CHANNELS 4U
#define TEMP_SLOTS (TEMP_GROUPS * TEMP_CHANNELS)
#define VALID_BITS_LEN ((CELL_SLOTS + 7U) / 8U)
#define TELEMETRY_FIXED_LEN 52U
#define TELEMETRY_PAYLOAD_LEN (TELEMETRY_FIXED_LEN + VALID_BITS_LEN + CELL_SLOTS * 2U + TEMP_SLOTS * 2U)

/* 单个串口的收帧状态。frame_buf 预留 2 字节 MAGIC + 前缀 + 最大载荷 + 2 字节 CRC。 */
typedef struct {
    UART_HandleTypeDef *uart;
    uint8_t rx_byte;                       /* HAL 逐字节接收的中转 */
    uint8_t frame_buf[2U + TELEMETRY_PREFIX_LEN + TELEMETRY_MAX_PAYLOAD + 2U];
    uint16_t frame_index;                  /* 已收字节数（0 = 未在拼帧） */
    uint16_t expected_frame_len;           /* 读到头后确定的总帧长（0 = 未知） */
} TelemetryPort;

/* RX 环形缓冲项：记录该字节来自哪个口 */
typedef struct {
    uint8_t port_index;
    uint8_t byte;
} RxRingItem;

static UART_HandleTypeDef *telemetry_uart = 0;   /* 当前活跃上位机口；0 = 无 */
static TelemetryPort telemetry_ports[TELEMETRY_PORT_COUNT];
static volatile RxRingItem rx_ring[TELEMETRY_RX_RING_SIZE];
static volatile uint16_t rx_head = 0;             /* 生产者(ISR)写 */
static volatile uint16_t rx_tail = 0;             /* 消费者(线程/主循环)写 */
static uint16_t tx_seq = 1;                       /* 发送序号，逐帧自增 */
static uint32_t last_telemetry_tx = 0;
static uint32_t last_host_heartbeat = 0;

#ifdef BMS_USE_RTTHREAD
/* RX 事件：每收到一个字节由中断释放一次，唤醒遥测线程出队处理 */
static struct rt_semaphore uart_rx_sem;
#endif

static uint16_t crc16_modbus(const uint8_t *data, uint16_t len);
static void process_byte(uint8_t port_index, uint8_t byte);
static void handle_frame(uint8_t port_index, const uint8_t *frame, uint16_t len);
static void send_frame(uint8_t msg_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len);
static void send_ack(uint16_t command_seq, uint8_t command_type, uint8_t result, uint8_t reason);
static int port_index_for_uart(UART_HandleTypeDef *huart);
static void request_bootloader_reset(void) __attribute__((noreturn));
static int cell_slot_valid(uint8_t group, uint8_t channel);
static void put_u8(uint8_t *payload, uint16_t *offset, uint8_t value);
static void put_u16(uint8_t *payload, uint16_t *offset, uint16_t value);
static void put_i16(uint8_t *payload, uint16_t *offset, int16_t value);
static void put_u32(uint8_t *payload, uint16_t *offset, uint32_t value);
static void put_i32(uint8_t *payload, uint16_t *offset, int32_t value);
static uint16_t get_u16(const uint8_t *data);
static int32_t get_i32(const uint8_t *data);

/* 初始化两个串口：建信号量（RTT）、登记 uart、复位索引，并对每个口开逐字节接收中断。 */
void telemetry_serial_init(UART_HandleTypeDef *uart1, UART_HandleTypeDef *uart2) {
#ifdef BMS_USE_RTTHREAD
    rt_sem_init(&uart_rx_sem, "uartrx", 0, RT_IPC_FLAG_FIFO);
#endif
    telemetry_ports[0].uart = uart1;
    telemetry_ports[1].uart = uart2;
    telemetry_uart = 0;
    rx_head = 0;
    rx_tail = 0;
    bms_host_active = 0;
    last_host_heartbeat = 0;
    for(uint8_t index = 0; index < TELEMETRY_PORT_COUNT; index++) {
        telemetry_ports[index].frame_index = 0;
        telemetry_ports[index].expected_frame_len = 0;
        if(telemetry_ports[index].uart != 0) {
            HAL_UART_Receive_IT(telemetry_ports[index].uart,
                                &telemetry_ports[index].rx_byte, 1);
        }
    }
}

/* 收字节中断：把字节入环形缓冲（满则丢弃），释放信号量（RTT），再重新武装接收。 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    int port_index = port_index_for_uart(huart);
    if(port_index >= 0) {
        uint16_t next_head = (uint16_t)((rx_head + 1U) % TELEMETRY_RX_RING_SIZE);
        if(next_head != rx_tail) {
            rx_ring[rx_head].port_index = (uint8_t)port_index;
            rx_ring[rx_head].byte = telemetry_ports[port_index].rx_byte;
            __DMB();                 /* 载荷先就绪，再发布索引 */
            rx_head = next_head;
        }
#ifdef BMS_USE_RTTHREAD
        rt_sem_release(&uart_rx_sem);
#endif
        HAL_UART_Receive_IT(huart, &telemetry_ports[port_index].rx_byte, 1);
    }
}

/* 串口错误中断：重新武装接收，保证链路能自恢复（不断流）。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    int port_index = port_index_for_uart(huart);
    if(port_index >= 0) {
        HAL_UART_Receive_IT(huart, &telemetry_ports[port_index].rx_byte, 1);
    }
}

/* 心跳超时判定：超过 LINK_TIMEOUT 无新心跳即判链路断开，清 host_active 并释放口。
 * 必须在**每拍都会调**的路径上跑（telemetry_serial_process），不能只挂在遥测发送路径里——
 * 否则上位机只发过一帧 HEARTBEAT 就断开时，[ST] 文本模式会被永久静默。 */
static void link_timeout_check(void) {
    if(bms_host_active && telemetry_uart != 0 &&
       HAL_GetTick() - last_host_heartbeat > TELEMETRY_LINK_TIMEOUT) {
        bms_host_active = 0;
        telemetry_uart = 0;
    }
}

/* 消费侧：排空 RX 环，逐字节喂给拼帧状态机。（裸机主循环每拍调） */
void telemetry_serial_process(void) {
    while(rx_tail != rx_head) {
        uint8_t port_index = rx_ring[rx_tail].port_index;
        uint8_t byte = rx_ring[rx_tail].byte;
        __DMB();                     /* 先读载荷，再推进出队索引 */
        rx_tail = (uint16_t)((rx_tail + 1U) % TELEMETRY_RX_RING_SIZE);
        if(port_index < TELEMETRY_PORT_COUNT) {
            process_byte(port_index, byte);
        }
    }
    link_timeout_check();
}

#ifdef BMS_USE_RTTHREAD
/* 遥测线程的等待（仅 RTT 版）：默认超时 = 上报周期；若上位机在线，则按"距下次上报的
 * 剩余时间"等待，避免 200ms 周期被量化到固定超时台阶；无字节时到点即醒，正好发下一帧。 */
void telemetry_serial_wait_next(void) {
    uint32_t timeout = TELEMETRY_TX_PERIOD;

    if(telemetry_uart != 0 && bms_host_active) {
        uint32_t elapsed = HAL_GetTick() - last_telemetry_tx;
        timeout = (elapsed >= TELEMETRY_TX_PERIOD) ? 1U
                                                   : (TELEMETRY_TX_PERIOD - elapsed);
    }
    rt_sem_take(&uart_rx_sem, rt_tick_from_millisecond(timeout));
}
#endif

int telemetry_host_active(void) {
    return bms_host_active ? 1 : 0;
}

/* 周期上报：无上位机则退出；到点才发。链路超时由 link_timeout_check() 统一处理。
 * 拼帧全程持状态锁取一致快照，拼完解锁再发（发送阻塞）。 */
void telemetry_serial_send_periodic(void) {
    if(telemetry_uart == 0 || !bms_host_active) {
        return;
    }

    uint32_t now = HAL_GetTick();
    if(now - last_telemetry_tx < TELEMETRY_TX_PERIOD) {
        return;
    }
    last_telemetry_tx = now;

    /* 锁内一次性取一致快照（ts_state/shunt/电芯/Bat_Volt），拼完帧解锁再发 */
    bms_state_lock();

    static uint8_t payload[TELEMETRY_PAYLOAD_LEN];
    memset(payload, 0, sizeof(payload));
    uint16_t offset = 0;

    /* 固定区：时间戳、状态机、错误、控制模式、AIR、SDC、HV_ACTIVE、电压电流温度等 */
    put_u32(payload, &offset, now);
    put_u8(payload, &offset, (uint8_t)ts_state.current_state);
    put_u8(payload, &offset, (uint8_t)ts_state.target_state);
    put_u32(payload, &offset, (uint32_t)ts_error_kind);
    put_u8(payload, &offset, (uint8_t)ts_sm_get_control_mode());
    put_u8(payload, &offset, ts_sm_get_air_bits());
    put_u8(payload, &offset, (uint8_t)(sdc_closed ? 1U : 0U));
    put_u8(payload, &offset,
           (uint8_t)(bms_in(HV_ACTIVE_GPIO_Port, HV_ACTIVE_Pin) == GPIO_PIN_SET ? 1U : 0U));
    put_i32(payload, &offset, Bat_Volt);
    put_i32(payload, &offset, shunt_data.voltage_bat);
    put_i32(payload, &offset, shunt_data.voltage_veh);
    put_i32(payload, &offset, shunt_data.current);
    put_i32(payload, &offset, shunt_data.busbartemp);
    put_u8(payload, &offset, (uint8_t)(current_soc + 0.5f));   /* SOC %，钳位已保证 0..100 */
    put_u16(payload, &offset, min_voltage);
    put_u16(payload, &offset, max_voltage);
    put_i16(payload, &offset, min_temp);
    put_i16(payload, &offset, max_temp);
    put_u16(payload, &offset, CELL_SLOTS);
    put_u16(payload, &offset, TEMP_SLOTS);
    put_u8(payload, &offset, TOTAL_IC);
    put_u16(payload, &offset, MIN_CELL_VOLT);
    put_u16(payload, &offset, MAX_CELL_VOLT);

    /* 有效位图：哪个 (group,channel) 槽位有真实电芯。位序=槽位序号，小端填位。
     * group 从 1 计，只有 group<=TOTAL_IC 且非空位才置位。 */
    for(uint8_t group = 1; group <= CELL_GROUPS; group++) {
        for(uint8_t channel = 1; channel <= CELL_CHANNELS; channel++) {
            uint16_t slot = (uint16_t)((group - 1U) * CELL_CHANNELS + (channel - 1U));
            if(group <= TOTAL_IC && cell_slot_valid(group, channel)) {
                payload[offset + slot / 8U] |= (uint8_t)(1U << (slot % 8U));
            }
        }
    }
    offset += VALID_BITS_LEN;

    /* 逐芯电压（mV，原始码 /10）。空位/超范围填 0。 */
    for(uint8_t group = 0; group < CELL_GROUPS; group++) {
        for(uint8_t channel = 0; channel < CELL_CHANNELS; channel++) {
            uint8_t one_based_group = (uint8_t)(group + 1U);
            uint8_t one_based_channel = (uint8_t)(channel + 1U);
            uint16_t value = 0;
            if(group < TOTAL_IC && cell_slot_valid(one_based_group, one_based_channel)) {
                value = (uint16_t)(cell_codes[group][channel] / 10U);
            }
            put_u16(payload, &offset, value);
        }
    }

    /* 逐温（℃）。超范围 IC 填 0。 */
    for(uint8_t group = 0; group < TEMP_GROUPS; group++) {
        for(uint8_t channel = 0; channel < TEMP_CHANNELS; channel++) {
            int16_t value = 0;
            if(group < TOTAL_IC) {
                value = temp[group][channel];
            }
            put_i16(payload, &offset, value);
        }
    }

    bms_state_unlock();
    send_frame(MSG_TELEMETRY_FULL, tx_seq++, payload, sizeof(payload));
}

/* 拼帧状态机（单口、逐字节）：
 *   0 字节：等 MAGIC0，否则丢。
 *   1 字节：等 MAGIC1；收到 MAGIC0 则重试（把当前字节当新首字节），否则复位。
 *   前缀读满（2+6）：校验 VER 与 LEN，算出 expected_frame_len（未知则 0）。
 *   收满 expected_frame_len：交给 handle_frame，复位。
 *   缓冲溢出保护：index 越界即复位。 */
static void process_byte(uint8_t port_index, uint8_t byte) {
    TelemetryPort *port = &telemetry_ports[port_index];

    if(port->frame_index == 0U) {
        if(byte != TELEMETRY_MAGIC0) {
            return;
        }
        port->frame_buf[port->frame_index++] = byte;
        return;
    }

    if(port->frame_index == 1U) {
        if(byte != TELEMETRY_MAGIC1) {
            /* 当前字节若是 MAGIC0，当作新的首字节重试，否则整体复位 */
            port->frame_index = (byte == TELEMETRY_MAGIC0) ? 1U : 0U;
            port->frame_buf[0] = TELEMETRY_MAGIC0;
            return;
        }
        port->frame_buf[port->frame_index++] = byte;
        return;
    }

    port->frame_buf[port->frame_index++] = byte;
    if(port->frame_index == 2U + TELEMETRY_PREFIX_LEN) {
        uint8_t version = port->frame_buf[2];
        uint16_t payload_len = get_u16(&port->frame_buf[6]);
        if(version != TELEMETRY_VERSION || payload_len > TELEMETRY_MAX_PAYLOAD) {
            port->frame_index = 0;
            port->expected_frame_len = 0;
            return;
        }
        port->expected_frame_len = (uint16_t)(2U + TELEMETRY_PREFIX_LEN + payload_len + 2U);
    }

    if(port->expected_frame_len > 0U && port->frame_index >= port->expected_frame_len) {
        handle_frame(port_index, port->frame_buf, port->expected_frame_len);
        port->frame_index = 0;
        port->expected_frame_len = 0;
    }

    if(port->frame_index >= sizeof(port->frame_buf)) {
        port->frame_index = 0;
        port->expected_frame_len = 0;
    }
}

/* 校验一帧（长度+CRC），必要时切换活跃上位机口，然后按 MSG_TYPE 分发命令。
 * 多数命令都会回一条 ACK（含被拒原因）。 */
static void handle_frame(uint8_t port_index, const uint8_t *frame, uint16_t len) {
    uint16_t payload_len = get_u16(&frame[6]);
    const uint8_t *payload = &frame[2U + TELEMETRY_PREFIX_LEN];
    uint16_t crc_offset = (uint16_t)(2U + TELEMETRY_PREFIX_LEN + payload_len);
    uint16_t received_crc = get_u16(&frame[crc_offset]);
    uint16_t computed_crc = crc16_modbus(&frame[2], (uint16_t)(TELEMETRY_PREFIX_LEN + payload_len));
    uint8_t msg_type = frame[3];
    uint16_t seq = get_u16(&frame[4]);

    /* 长度不符或 CRC 错，整帧丢弃 */
    if(len != crc_offset + 2U || received_crc != computed_crc) {
        return;
    }

    /* 端口归属：当前无活跃口 → 认这个口；当前有活跃口且其心跳未超时 → 忽略别口的帧；
     * 有活跃口但已超时 → 抢占切换到本口。切换后先清 bms_host_active，等本口心跳。 */
    UART_HandleTypeDef *source_uart = telemetry_ports[port_index].uart;
    uint32_t now = HAL_GetTick();
    if(telemetry_uart != source_uart) {
        if(telemetry_uart != 0 && bms_host_active &&
           (now - last_host_heartbeat <= TELEMETRY_LINK_TIMEOUT)) {
            return;
        }
        telemetry_uart = source_uart;
        bms_host_active = 0;
    }

    if(msg_type == MSG_SET_TARGET_STATE) {
        /* DATA=[state]，只允许 INACTIVE/ACTIVE/CHARGE；被拒 reason=1 */
        if(payload_len != 1U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        int ok = ts_sm_serial_set_target((TSState)payload[0]);
        send_ack(seq, msg_type, ok ? ACK_OK : ACK_REJECTED, ok ? 0U : 1U);
    }
    else if(msg_type == MSG_SET_MANUAL_AIR) {
        /* DATA=[enable, bits]：调试用手动吸合继电器 */
        if(payload_len != 2U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        int ok = ts_sm_serial_set_manual_air(payload[0] != 0U, payload[1]);
        send_ack(seq, msg_type, ok ? ACK_OK : ACK_REJECTED, ok ? 0U : 1U);
    }
    else if(msg_type == MSG_HEARTBEAT) {
        /* 空载荷：标记本口为活跃上位机并刷新时间戳。无 ACK。 */
        if(payload_len == 0U) {
            bms_host_active = 1;
            last_host_heartbeat = now;
            ts_sm_serial_heartbeat();
        }
    }
    else if(msg_type == MSG_ENTER_BOOTLOADER) {
        /* 空载荷：回 ACK 后延时 20ms 让 ACK 发完，再写备份寄存器并复位进 bootloader。 */
        if(payload_len != 0U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        send_ack(seq, msg_type, ACK_OK, 0);
        BMS_DELAY_MS(20U);
        request_bootloader_reset();
    }
    else if(msg_type == MSG_CLEAR_ERRORS) {
        /* 空载荷：一次性清错（监视器仍在跑，故障未消会立即重报）。 */
        if(payload_len != 0U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        int ok = ts_sm_serial_clear_errors();
        send_ack(seq, msg_type, ok ? ACK_OK : ACK_REJECTED, ok ? 0U : 1U);
    }
#if BMS_TEST_HOOKS
    else if(msg_type == MSG_SET_INPUT) {
        /* DATA = [sig_id, value]，逻辑验证用：注入 BMS 数字量输入 */
        if(payload_len != 2U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        int ok = (bms_io_inject(payload[0], payload[1]) == 0);
        send_ack(seq, msg_type, ok ? ACK_OK : ACK_REJECTED, ok ? 0U : 1U);
    }
    else if(msg_type == MSG_DUMP_IO) {
        /* 空载荷：回 ACK + 逐行 [DUMP]（force 打印，会话中也会输出）。 */
        if(payload_len != 0U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        bms_io_dump();
        send_ack(seq, msg_type, ACK_OK, 0U);
    }
    else if(msg_type == MSG_CAN_SELFTEST) {
        /* DATA = [enable]，回环自测开关 */
        if(payload_len != 1U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        can_custom_selftest(payload[0] != 0U);
        send_ack(seq, msg_type, ACK_OK, 0U);
    }
    else if(msg_type == MSG_INJECT) {
        /* DATA = [item, i32 value LE]，故障注入（逻辑验证用，item 见 bms_io.h） */
        if(payload_len != 5U) {
            send_ack(seq, msg_type, ACK_BAD_LENGTH, 0);
            return;
        }
        uint8_t item = payload[0];
        int32_t value = get_i32(&payload[1]);
        int ok;
        /* item 落在哪个域，就分派给哪个监视器的注入口 */
        if(item <= BMS_FI_SHUNT_MAX) {
            ok = (shunt_inject(item, value) == 0);
        }
        else if(item >= BMS_FI_AFE_MIN && item <= BMS_FI_AFE_MAX) {
            ok = (slaves_inject(item, value) == 0);
        }
        else if(item == BMS_FI_CHARGE_MODE) {
            ok = (charge_inject(value) == 0);
        }
        else {
            ok = 0;
        }
        send_ack(seq, msg_type, ok ? ACK_OK : ACK_REJECTED, ok ? 0U : 1U);
    }
#else
    /* 量产固件：0x20~0x23 是台架自测/故障注入口，一律拒绝。
     * 不保留任何能覆盖真实输入(sdc_closed/引脚)或改写 AFE/分流器读数的路径。 */
    else if(msg_type == MSG_SET_INPUT || msg_type == MSG_DUMP_IO ||
            msg_type == MSG_CAN_SELFTEST || msg_type == MSG_INJECT) {
        send_ack(seq, msg_type, ACK_REJECTED, 0);
    }
#endif
    else {
        send_ack(seq, msg_type, ACK_UNKNOWN_COMMAND, 0);
    }
}

/* 按 UART 句柄反查端口序号；未登记返回 -1。 */
static int port_index_for_uart(UART_HandleTypeDef *huart) {
    for(uint8_t index = 0; index < TELEMETRY_PORT_COUNT; index++) {
        if(telemetry_ports[index].uart == huart) {
            return index;
        }
    }
    return -1;
}

/* 写备份寄存器请求重入 bootloader 并软复位。BOOT0R/BKP1R 互为反码作为双保险，
 * 让 bootloader 能识别"这是 APP 主动请求升级"。__attribute__((noreturn)) + unreachable。 */
static void request_bootloader_reset(void) {
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    RTC->BKP0R = BOOT_REQUEST_MAGIC;
    RTC->BKP1R = ~BOOT_REQUEST_MAGIC;
    __DSB();
    NVIC_SystemReset();
    __builtin_unreachable();
}

/* 回 ACK：payload=[命令序号(2), 命令类型(1), result(1), reason(1)]。 */
static void send_ack(uint16_t command_seq, uint8_t command_type, uint8_t result, uint8_t reason) {
    uint8_t payload[5];
    payload[0] = (uint8_t)(command_seq & 0xFFU);
    payload[1] = (uint8_t)(command_seq >> 8);
    payload[2] = command_type;
    payload[3] = result;
    payload[4] = reason;
    send_frame(MSG_ACK, tx_seq++, payload, sizeof(payload));
}

/* 组帧并发送：MAGIC + VER + TYPE + SEQ(2) + LEN(2) + DATA + CRC16(2)。
 * TX 走 bms_tx_lock，避免与文本日志在串口上交错。 */
static void send_frame(uint8_t msg_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len) {
    if(telemetry_uart == 0 || payload_len > TELEMETRY_MAX_PAYLOAD) {
        return;
    }

    static uint8_t frame[2U + TELEMETRY_PREFIX_LEN + TELEMETRY_MAX_PAYLOAD + 2U];
    uint16_t offset = 0;
    frame[offset++] = TELEMETRY_MAGIC0;
    frame[offset++] = TELEMETRY_MAGIC1;
    frame[offset++] = TELEMETRY_VERSION;
    frame[offset++] = msg_type;
    frame[offset++] = (uint8_t)(seq & 0xFFU);
    frame[offset++] = (uint8_t)(seq >> 8);
    frame[offset++] = (uint8_t)(payload_len & 0xFFU);
    frame[offset++] = (uint8_t)(payload_len >> 8);
    if(payload_len > 0U) {
        memcpy(&frame[offset], payload, payload_len);
        offset = (uint16_t)(offset + payload_len);
    }
    uint16_t crc = crc16_modbus(&frame[2], (uint16_t)(TELEMETRY_PREFIX_LEN + payload_len));
    frame[offset++] = (uint8_t)(crc & 0xFFU);
    frame[offset++] = (uint8_t)(crc >> 8);
    bms_tx_lock();
    HAL_UART_Transmit(telemetry_uart, frame, offset, 100);
    bms_tx_unlock();
}

/* 槽位是否有效：偶数 group（2,4,...,12）的第 12 通道是空位（对应 "Skip No.24 cell"）。 */
static int cell_slot_valid(uint8_t group, uint8_t channel) {
    return !(group % 2U == 0U && channel == CELL_CHANNELS);
}

/* CRC16-Modbus：初值 0xFFFF、多项式 0xA001（反射），与上位机一致。 */
static uint16_t crc16_modbus(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFU;
    for(uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t bit = 0; bit < 8U; bit++) {
            if((crc & 1U) != 0U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ---- 小端序列化辅助（无符号与有符号同形，仅位宽不同）---- */
static void put_u8(uint8_t *payload, uint16_t *offset, uint8_t value) {
    payload[(*offset)++] = value;
}

static void put_u16(uint8_t *payload, uint16_t *offset, uint16_t value) {
    payload[(*offset)++] = (uint8_t)(value & 0xFFU);
    payload[(*offset)++] = (uint8_t)(value >> 8);
}

static void put_i16(uint8_t *payload, uint16_t *offset, int16_t value) {
    put_u16(payload, offset, (uint16_t)value);
}

static void put_u32(uint8_t *payload, uint16_t *offset, uint32_t value) {
    payload[(*offset)++] = (uint8_t)(value & 0xFFU);
    payload[(*offset)++] = (uint8_t)((value >> 8) & 0xFFU);
    payload[(*offset)++] = (uint8_t)((value >> 16) & 0xFFU);
    payload[(*offset)++] = (uint8_t)((value >> 24) & 0xFFU);
}

static void put_i32(uint8_t *payload, uint16_t *offset, int32_t value) {
    put_u32(payload, offset, (uint32_t)value);
}

/* 反序列化：只用到 u16（LEN/CRC/SEQ）与 i32（注入值）。 */
static uint16_t get_u16(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int32_t get_i32(const uint8_t *data) {
    return (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                     ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
}
