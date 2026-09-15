#ifndef STUB_SHUNT_MONITOR_H
#define STUB_SHUNT_MONITOR_H

/* 宿主机单测用桩：只保留 soc_estimation.c 实际读到的两个字段。 */

#include <stdint.h>

typedef struct {
    int32_t current;           /* mA，充电为正、放电为负 */
    float   current_counter;   /* mAs，安时积分累计 */
} ShuntData;

extern ShuntData shunt_data;

#endif /* STUB_SHUNT_MONITOR_H */
