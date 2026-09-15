/* ==========================================================================
 * ring_test.c —— SPSC 环形缓冲的宿主机并发测试
 *
 * 做法：一条生产线程（模拟 CAN RX ISR）× 一条消费线程（模拟 bms_can_rx），
 * 载荷里塞入"序号 + 按位取反"，消费侧逐帧校验：
 *   - 校验和：data[4..7] 必须等于 ~data[0..3]  -> 抓"载荷没写完就被读"（撕裂）
 *   - id 一致：帧 id 必须等于序号
 *   - 严格递增：序号必须逐 1 递增、不重不乱序     -> 抓"重复消费/乱序/索引被绕过"
 * 结束后再对总账：consumed + drops == total        -> 抓"丢帧计数漏记/多发"
 *
 * 两种模式：
 *   ./ring_test          正常：期望 PASS
 *   ./ring_test fault    负对照：人为把索引提前发布，期望 FAIL（证明校验有牙）
 *
 * 注意（务实说明）：在 x86 上，硬件内存模型是 TSO，本身就比较强，
 * 这个压力测试**不能**可靠地暴露"内存序写错"这类问题——那需要 TSan/Linux。
 * 它能可靠覆盖的是确定性逻辑（容量/回绕/丢帧记账/载荷完整性）。
 * ========================================================================== */
#include "ring_port.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUNDS   8
#define PER_ROUND 200000L

static atomic_int producer_done;
static long       total_checks;
static long       total_fails;
static long       print_budget = 10;   /* 只打前若干条失败，避免刷屏 */

/* 生产线程：把 seq 编进载荷并连发 PER_ROUND 帧。 */
static void *producer(void *arg) {
    long total = *(long *)arg;
    long seq;

    for (seq = 0; seq < total; seq++) {
        uint32_t s = (uint32_t)seq;
        uint32_t c = ~s;
        uint8_t d[8];
        d[0] = (uint8_t)(s      );
        d[1] = (uint8_t)(s >>  8);
        d[2] = (uint8_t)(s >> 16);
        d[3] = (uint8_t)(s >> 24);
        d[4] = (uint8_t)(c      );
        d[5] = (uint8_t)(c >>  8);
        d[6] = (uint8_t)(c >> 16);
        d[7] = (uint8_t)(c >> 24);
        ring_port_produce(s, d, sizeof(d));

        sched_yield();              /* 每帧都让出：让消费侧跟得上，避免全是丢帧 */
    }

    atomic_store_explicit(&producer_done, 1, memory_order_release);
    return NULL;
}

/* 校验一帧；expected 是消费侧见过的最大序号+1。
 * 返回该帧序号（异常返回 -1）。
 * 注意：生产者丢帧时消费侧会看到序号**跳空**，这是合法的（对应 drops），
 * 因此只要求"严格递增"——序号 <= expected 才算重复/乱序。 */
static long verify_frame(const bms_can_msg_t *m, long expected) {
    uint32_t s, c, want_c;
    long     seq;

    total_checks++;

    memcpy(&s, m->data, 4);         /* 小端：与生产者写入一致 */
    memcpy(&c, m->data + 4, 4);
    want_c = ~s;
    seq = (long)s;

    if (m->dlc != 8U) {
        if (print_budget-- > 0) printf("    FAIL: dlc=%u (want 8)\n", (unsigned)m->dlc);
        return -1;
    }
    if (c != want_c) {              /* 载荷撕裂 / 未写完 */
        if (print_budget-- > 0)
            printf("    FAIL: torn payload seq=%ld got_c=0x%08lX want_c=0x%08lX\n",
                   seq, (unsigned long)c, (unsigned long)want_c);
        return -1;
    }
    if ((long)m->id != seq) {
        if (print_budget-- > 0)
            printf("    FAIL: id=%lu != seq=%ld\n", (unsigned long)m->id, seq);
        return -1;
    }
    if (seq < expected) {           /* 重复消费 / 乱序 */
        if (print_budget-- > 0)
            printf("    FAIL: seq=%ld < last+1=%ld (dup/reorder)\n", seq, expected);
        return -1;
    }
    return seq;
}

/* 消费线程：一直取，直到生产侧宣告结束且环空。 */
static void *consumer(void *arg) {
    long expected = 0;
    long consumed = 0;
    long fails = 0;
    bms_can_msg_t m;

    (void)arg;

    for (;;) {
        if (ring_port_pop(&m)) {
            long got = verify_frame(&m, expected);
            if (got < 0) {
                fails++;                 /* 记账但不中断：要继续排空，否则环会卡住 */
            } else {
                expected = got + 1;
            }
            consumed++;
            if ((consumed & 0x3F) == 0) {
                sched_yield();            /* 比生产慢，制造回压/丢帧 */
            }
            continue;
        }

        if (atomic_load_explicit(&producer_done, memory_order_acquire)) {
            if (ring_port_pop(&m)) {
                long got = verify_frame(&m, expected);
                if (got < 0) fails++;
                else expected = got + 1;
                consumed++;
                continue;
            }
            break;
        }
        sched_yield();
    }

    total_fails += fails;
    return (void *)(intptr_t)consumed;
}

int main(int argc, char **argv) {
    int fault = (argc > 1 && strcmp(argv[1], "fault") == 0);
    int round;

    ring_fault_publish_early = fault;

    printf("=== ring_test (%s mode) ===\n",
           fault ? "FAULT-INJECT publish-early" : "correct");

    for (round = 0; round < ROUNDS; round++) {
        pthread_t pt, ct;
        void     *cret = NULL;
        long      produced = PER_ROUND;
        long      consumed;
        uint32_t  drops;

        ring_port_reset();
        atomic_store(&producer_done, 0);

        pthread_create(&pt, NULL, producer, &produced);
        pthread_create(&ct, NULL, consumer, NULL);

        pthread_join(pt, NULL);
        pthread_join(ct, &cret);
        consumed = (long)(intptr_t)cret;
        drops = ring_port_drop_count();

        if (consumed + (long)drops != produced) {
            printf("  round %d: FAIL accounting consumed=%ld drops=%u total=%ld\n",
                   round, consumed, (unsigned)drops, produced);
            total_fails++;
        } else {
            printf("  round %d: ok  consumed=%ld drops=%u\n",
                   round, consumed, (unsigned)drops);
        }
    }

    printf("--- checks=%ld fails=%ld ---\n", total_checks, total_fails);

    if (fault) {
        /* 负对照：注入故障就必须被抓到，否则说明校验形同虚设 */
        if (total_fails > 0) {
            printf("RESULT: fault detected as expected (harness has teeth)\n");
            return 0;
        }
        printf("RESULT: NEGATIVE CONTROL FAILED - fault not detected!\n");
        return 2;
    }

    if (total_fails == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL (%ld)\n", total_fails);
    return 1;
}
