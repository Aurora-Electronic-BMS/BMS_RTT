#ifndef STUB_STM32F4XX_HAL_H
#define STUB_STM32F4XX_HAL_H

/* 宿主机单测用桩：只提供 soc_estimation.c 用到的 HAL_GetTick。
 * 由 soc_stubs.c 实现，返回值由 soc_test_set_tick() 驱动。 */

#include <stdint.h>

uint32_t HAL_GetTick(void);

#endif /* STUB_STM32F4XX_HAL_H */
