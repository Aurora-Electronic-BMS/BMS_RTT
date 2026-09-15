#ifndef STUB_SLAVE_MONITOR_H
#define STUB_SLAVE_MONITOR_H

/* 宿主机单测用桩：只保留 soc_estimation.c 用到的 min_voltage。 */

#include <stdint.h>

extern uint16_t min_voltage;   /* mV，最低单体电压 */

#endif /* STUB_SLAVE_MONITOR_H */
