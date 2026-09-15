#ifndef RING_PORT_H
#define RING_PORT_H

/* ==========================================================================
 * ring_port.h —— can_custom.c 里 CAN RX 环形缓冲的宿主机移植
 *
 * 目的：把固件里那段"ISR 入队 / 线程出队"的 SPSC 环形缓冲搬到宿主机，
 * 用 pthread 造真并发，交给检测器（TSan）或压力+不变量校验去查竞态。
 *
 * 与固件的**逐条对应**（can_custom.c）：
 *   固件 volatile bms_can_msg_t can_ring[BMS_CAN_RING_SIZE]
 *     -> 这里 ring[]，改用 _Atomic 索引表达固件 __DMB() 想表达的
 *        "载荷先就绪、索引后发布"（release/acquire），而非依赖 volatile。
 *   固件 (w + 1) % SIZE == r 判满
 *     -> 原样保留：可用容量 = SIZE-1 = 63（不是 64），这是固件的既有行为。
 *   固件 can_ring_drop
 *     -> ring_drop。
 *
 * 为什么用 _Atomic 而不是 volatile：
 *   volatile 只保证"不被优化掉"，不提供原子性与跨线程可见性；在单核
 *   Cortex-M 上配合 __DMB() 恰好能用，但搬到多核宿主机就不再成立。
 *   _Atomic + release/acquire 才是这段意图在 C11 内存模型下的正确表达。
 * ========================================================================== */

#include <stdint.h>
#include <stddef.h>

#define BMS_CAN_RING_SIZE 64

typedef struct {
    uint8_t  bus;     /* 1=CAN1, 2=CAN2 */
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} bms_can_msg_t;

/* 故障注入开关（负对照用）：
 *   0 = 正常，载荷写完再 release 发布索引（等价固件 __DMB__ 的位置）；
 *   1 = 故意把索引**提前**发布，再写载荷（等价于 __DMB__ 放错位置 / 缺屏障）。
 * 期望：0 下测试通过；1 下测试必须报 FAIL —— 用来证明这套校验"有牙"，
 * 而不是无论如何都绿灯。 */
extern int ring_fault_publish_early;

void     ring_port_reset(void);
void     ring_port_produce(uint32_t id, const uint8_t *data, size_t datalen); /* 生产侧(ISR) */
int      ring_port_pop(bms_can_msg_t *out);                                   /* 消费侧(线程) */
uint32_t ring_port_drop_count(void);

#endif /* RING_PORT_H */
