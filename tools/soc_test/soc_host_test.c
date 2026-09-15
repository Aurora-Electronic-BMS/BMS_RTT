/* ==========================================================================
 * soc_host_test.c —— SOC 估算的宿主机单测
 *
 * 做法：不复制算法，直接编译**真实的 soc_estimation.c**（RT 版或裸机版，由 run.sh
 * 选），只把上游依赖（shunt_data / min_voltage / HAL_GetTick）换成 soc_stubs.c 里的
 * 假实现，然后按物理预期断言 current_soc：
 *   - 播种：静置时用 min_voltage 查 OCV-SOC 表定初值
 *   - 充电（正电流）SOC 上升；放电（负电流）SOC 下降 —— 方向与量级都要对
 *   - 钳位到 [0,100]
 *   - 静置够久用 OCV 重校
 *   - min_voltage 未就绪时不出数
 *   - 负对照：把**原始缺陷公式**内联一份，断言它给出与预期不同的值，证明断言"有牙"
 *
 * 期望值来源（与固件常量一致）：
 *   CELL_CAPACITY = 50760000 mAs，故 5,076,000 mAs = 满量程的 10% = 10 个百分点
 *   OCV 表：3796mV→50%，3721mV→20%
 * ========================================================================== */
#include <stdint.h>
#include <stdio.h>

#include "soc_estimation.h"
#include "shunt_monitor.h"
#include "slave_monitor.h"

void soc_test_set_tick(uint32_t t);

static int checks;
static int fails;

/* 不用 libm：手写绝对值差，避免依赖 -lm。 */
static float fdiff(float a, float b) { return (a > b) ? (a - b) : (b - a); }

static void expect_near(const char *what, float got, float want, float tol) {
    checks++;
    if(fdiff(got, want) > tol) {
        printf("  FAIL %-34s got=%.3f want=%.3f\n", what, got, want);
        fails++;
    }
    else {
        printf("  ok   %-34s = %.3f\n", what, got);
    }
}

/* 每个场景前重置到确定状态：soc_init + 设定 min_voltage / 时刻 / 瞬时电流 / 累计电量。 */
static void reset(uint32_t mv, uint32_t tick, int32_t current, float counter) {
    soc_init();
    min_voltage = (uint16_t)mv;
    soc_test_set_tick(tick);
    shunt_data.current = current;
    shunt_data.current_counter = counter;
}

/* 原始（缺陷）实现副本，仅用于负对照。照抄 BMS_V1.0 修正前的逻辑。 */
static float orig_buggy(float base_soc, float base_mAs, float counter, int32_t current) {
    if(current >= 200) {                              /* 只认充电方向 */
        return base_soc;
    }
    float mAs_delta = counter - base_mAs;
    return base_soc - mAs_delta / 50760000.0f * 100.0f;   /* 符号反 */
}

int main(void) {
    printf("=== soc_host_test ===\n");

    /* 1) 播种：静置 3796mV → 50% */
    reset(3796, 1000, 0, 0.0f);
    soc_update();
    expect_near("seed @3796mV (静置 OCV)", current_soc, 50.0f, 0.01f);

    /* 2) 充电：+10% 电量 → SOC 50 → 60 */
    reset(3796, 1000, 10000, 0.0f);
    soc_update();                                    /* 播种 50%，记基准 */
    shunt_data.current_counter = 5076000.0f;         /* +10% 满量程 */
    soc_test_set_tick(2000);
    soc_update();
    expect_near("charge +10% (正电流→上升)", current_soc, 60.0f, 0.05f);

    /* 3) 放电：-10% 电量 → SOC 50 → 40（抓"只认充电/符号反"两个缺陷） */
    reset(3796, 1000, -10000, 0.0f);
    soc_update();
    shunt_data.current_counter = -5076000.0f;
    soc_test_set_tick(2000);
    soc_update();
    expect_near("discharge -10% (负电流→下降)", current_soc, 40.0f, 0.05f);

    /* 4) 上限钳位 */
    reset(3796, 1000, 10000, 0.0f);
    soc_update();
    shunt_data.current_counter = 60000000.0f;
    soc_test_set_tick(2000);
    soc_update();
    expect_near("clamp high (+118%→100)", current_soc, 100.0f, 0.001f);

    /* 5) 下限钳位 */
    reset(3796, 1000, -10000, 0.0f);
    soc_update();
    shunt_data.current_counter = -60000000.0f;
    soc_test_set_tick(2000);
    soc_update();
    expect_near("clamp low (−118%→0)", current_soc, 0.0f, 0.001f);

    /* 6) 静置重校：先充到 60%，再静置 > 100s，OCV 拉回 3721mV→20% */
    reset(3796, 1000, 10000, 0.0f);
    soc_update();
    shunt_data.current_counter = 5076000.0f;
    soc_test_set_tick(2000);
    soc_update();
    expect_near("pre-idle soc", current_soc, 60.0f, 0.05f);

    shunt_data.current = 0;                          /* 转静置 */
    min_voltage = 3721;                              /* OCV 20% */
    soc_test_set_tick(2000u + 100001u);              /* 静置 > NO_CURRENT_TIME */
    soc_update();
    expect_near("idle resync → OCV 20%", current_soc, 20.0f, 0.01f);

    /* 7) min_voltage 未就绪（0xFFFF）时不出数：保持 soc_init 后的初值 */
    reset(0xFFFF, 1000, 0, 0.0f);
    soc_update();
    expect_near("no seed when minV invalid", current_soc, 0.0f, 0.001f);

    /* 8) 负对照：原始缺陷公式在场景 2/3 下必须给出与预期不同的值 */
    {
        float b_charge = orig_buggy(50.0f, 0.0f, 5076000.0f, 10000);    /* 正确应为 60 */
        float b_dis    = orig_buggy(50.0f, 0.0f, -5076000.0f, -10000);  /* 正确应为 40 */
        checks += 2;
        if(fdiff(b_charge, 60.0f) < 0.05f) {
            printf("  FAIL neg-control charge: 缺陷公式竟给出 %.3f（断言没有区分力）\n", b_charge);
            fails++;
        }
        else {
            printf("  ok   neg-control charge 缺陷公式=%.3f ≠ 60（有区分力）\n", b_charge);
        }
        if(fdiff(b_dis, 40.0f) < 0.05f) {
            printf("  FAIL neg-control discharge: 缺陷公式竟给出 %.3f（断言没有区分力）\n", b_dis);
            fails++;
        }
        else {
            printf("  ok   neg-control discharge 缺陷公式=%.3f ≠ 40（有区分力）\n", b_dis);
        }
    }

    printf("--- checks=%d fails=%d ---\n", checks, fails);
    if(fails == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL (%d)\n", fails);
    return 1;
}
