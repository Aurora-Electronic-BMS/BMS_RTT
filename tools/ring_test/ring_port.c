/* ring_port.c —— SPSC 环形缓冲移植实现（对应 can_custom.c 的入队/出队） */
#include "ring_port.h"

#include <stdatomic.h>

int ring_fault_publish_early = 0;

/* 载荷本身保持普通内存（固件里也是普通数组），
 * 靠索引上的 release/acquire 建立 happens-before 来保证"读到的载荷一定写完"。 */
static bms_can_msg_t ring[BMS_CAN_RING_SIZE];
static atomic_uint   ring_w;      /* 写索引，只有生产侧写 */
static atomic_uint   ring_r;      /* 读索引，只有消费侧写 */
static atomic_uint   ring_drop;

void ring_port_reset(void) {
    atomic_store(&ring_w, 0U);
    atomic_store(&ring_r, 0U);
    atomic_store(&ring_drop, 0U);
}

/* 生产一帧（对应 ftcan_msg_received_cb 的入队段）。
 * 满则累加丢弃计数并返回，其余情况写入 slot 后发布写索引。 */
void ring_port_produce(uint32_t id, const uint8_t *data, size_t datalen) {
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    unsigned next = (w + 1U) % BMS_CAN_RING_SIZE;
    unsigned n, i;

    /* 判满：固件用 (w+1)%SIZE == r，故可用容量是 SIZE-1 */
    if (next == atomic_load_explicit(&ring_r, memory_order_acquire)) {
        atomic_fetch_add_explicit(&ring_drop, 1U, memory_order_relaxed);
        return;
    }

    /* 负对照：索引提前发布，暴露"载荷尚未写完就被消费" */
    if (ring_fault_publish_early) {
        atomic_store_explicit(&ring_w, next, memory_order_release);
    }

    ring[w].bus = 1U;
    ring[w].id = id;
    n = (datalen > 8U) ? 8U : (unsigned)datalen;
    ring[w].dlc = (uint8_t)n;
    for (i = 0U; i < n; i++) {
        ring[w].data[i] = data[i];
    }

    if (!ring_fault_publish_early) {
        /* release：保证上面的载荷写入对"读到本索引的消费侧"可见 */
        atomic_store_explicit(&ring_w, next, memory_order_release);
    }
}

/* 出队一帧（对应 can_custom_msg_pop）。空返回 0。 */
int ring_port_pop(bms_can_msg_t *out) {
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);

    /* acquire：看到写索引前进，即保证相应载荷已就绪 */
    if (r == atomic_load_explicit(&ring_w, memory_order_acquire)) {
        return 0;
    }

    *out = ring[r];

    /* release：本 slot 已读完，生产者可安全复用 */
    atomic_store_explicit(&ring_r, (r + 1U) % BMS_CAN_RING_SIZE,
                          memory_order_release);
    return 1;
}

uint32_t ring_port_drop_count(void) {
    return atomic_load(&ring_drop);
}
