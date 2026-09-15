/* soc_stubs.c —— 宿主机单测的桩实现：把固件依赖（shunt_data / min_voltage /
 * HAL_GetTick）替换成测试可控的假实现，好让真实的 soc_estimation.c 能直接编译运行。 */
#include "shunt_monitor.h"
#include "slave_monitor.h"
#include "stm32f4xx_hal.h"

ShuntData shunt_data;
uint16_t  min_voltage;

static uint32_t g_tick;

uint32_t HAL_GetTick(void) { return g_tick; }

/* 由测试推进"系统时间"（ms）。 */
void soc_test_set_tick(uint32_t t) { g_tick = t; }
