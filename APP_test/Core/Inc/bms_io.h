#ifndef __BMS_IO_H
#define __BMS_IO_H

/* ==========================================================================
 * bms_io.h —— BMS 数字 IO 的虚拟层接口（逻辑验证用）
 *
 * 提供两组能力：
 *   ① 虚拟引脚读写：bms_out/bms_in 取代 HAL_GPIO_WritePin/ReadPin。输出只记录
 *      电平并在变化时打印；输入返回值由注入决定（真实引脚未接线读回来无意义）。
 *   ② 两把锁：tx_mtx（串口打印/遥测发送互斥）与 state_mtx（跨线程共享状态互斥，
 *      递归锁）。铁律：绝不跨阻塞调用持锁。
 * 详见 bms_io.c。
 * ========================================================================== */

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 可注入输入信号（对应 BMS 数字量输入）。0=POWER_UP, 1=CHARGE_ENABLE, 2=SDC_VOLTAGE */
#define BMS_SIG_POWER_UP      0U
#define BMS_SIG_CHARGE_ENABLE 1U
#define BMS_SIG_SDC_VOLTAGE   2U
#define BMS_SIG_COUNT         3U

/* ---- 故障注入 item 空间（串口命令 MSG_INJECT=0x23，payload=[item, i32 LE]）----
 * 用来在没有真实 BMS 硬件时触发各条保护分支。分三个域：
 *   shunt 0x0x：直接改写 shunt_data（绕过 CAN），并顺带刷新 last_message，
 *              避免叠加 SHUNT_TIMEOUT；因此要测超时需停止注入而不是注入 0。
 *   AFE   0x1x：在 slave_monitor 层叠加覆盖 LTC6804 桩件读数，value=-1 撤销。
 *   charge 0x2x：模拟收到充电机 CCS 帧（charge_mode 1s 后自动过期，需重发）。 */
#define BMS_FI_SHUNT_CURRENT      0x00U   /* i32 mA：< -200000 或 > 15000 触发超流 */
#define BMS_FI_SHUNT_VOLTAGE_BAT  0x01U   /* i32 mV：电池侧，参与电压失配/预充判定 */
#define BMS_FI_SHUNT_VOLTAGE_VEH  0x02U   /* i32 mV：车辆侧，参与预充判定 */
#define BMS_FI_SHUNT_BUSBAR_TEMP  0x03U   /* i32 0.1℃：> 1000 触发分流器过温 */
#define BMS_FI_SHUNT_MAX          0x0FU
#define BMS_FI_AFE_CELL_RAW       0x10U   /* i32 原始 tc 码(mV*10)：> 42000 过压，< 30000 欠压 */
#define BMS_FI_AFE_AUX_RAW        0x11U   /* i32 AUX 原始值：经 temp_cal 换算温度 */
#define BMS_FI_AFE_PEC            0x12U   /* 0=正常 255=坏；坏且电流<-1000mA 触发内部通信错误 */
#define BMS_FI_AFE_MIN            0x10U
#define BMS_FI_AFE_MAX            0x1FU
#define BMS_FI_CHARGE_MODE        0x20U   /* 0/1：模拟充电机 CCS 帧，令 POWER_UP 目标为 CHARGE */

/* 虚拟 IO：替代 HAL_GPIO_WritePin / HAL_GPIO_ReadPin，做逻辑验证 */
void bms_io_init(void);
void bms_out(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState bms_in(GPIO_TypeDef *port, uint16_t pin);

/* 输入注入：由串口命令调用。返回 0 成功，-1 未知信号 */
int bms_io_inject(uint8_t sig, uint8_t value);
void bms_io_dump(void);

/* TX 互斥：串口打印与遥测发送共用一把锁，避免帧交错 */
void bms_tx_lock(void);
void bms_tx_unlock(void);

/* 状态互斥：保护跨线程共享的 BMS 状态（sig_tab、shunt/slave 数据、ts_state
 * 多字段更新、error_monitor）。**递归锁**——叶子函数自锁、外层再包一层不会自锁。
 * 铁律：绝不跨阻塞调用持锁（rt_thread_mdelay / HAL_UART_Transmit 一律在锁外）。 */
void bms_state_lock(void);
void bms_state_unlock(void);

/* 上位机会话是否活跃（由 telemetry_serial.c 维护）。
 * 活跃时 bms_log 自动静默，保持 B5 4D 二进制流干净；bms_log_force 强制输出
 * （供上位机主动请求的 [DUMP] 用）。 */
extern volatile int bms_host_active;

/* 多线程安全日志（局部格式化缓冲 + TX 互斥输出） */
void bms_log(const char *fmt, ...);
void bms_log_force(const char *fmt, ...);

#endif /* __BMS_IO_H */
