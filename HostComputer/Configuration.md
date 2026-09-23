# BMS 上位机配置功能说明

## 1. 功能概述

本功能使上位机能够读取、修改并保存 BMS 的运行参数，不需要在每次调整后修改 C 宏、重新编译或重新烧录。

首次使用前，必须先升级包含本功能的 APP 固件。之后，参数由上位机写入 STM32F407 的内部 Flash；正常断电或复位后，固件在启动时自动加载最近一次校验通过的配置。

> 配置只能在 BMS 为 `INACTIVE` 且 POS/NEG/PRE 三个 AIR 均断开时保存。此限制由固件执行，不能仅靠上位机界面绕过。

## 2. 上位机操作

1. 连接 BMS，等待状态栏显示 `Connected`。
2. 点击连接栏中的 **Configuration**。
3. 上位机先发送读取命令，并以设备当前实际生效的配置填充窗口。
4. 修改需要调整的字段。
5. 确认 BMS 为 `INACTIVE`、所有 AIR 已断开后，点击 **Save**。
6. 等待状态栏提示 `Configuration saved to BMS Flash.`。
7. 若修改了 CAN1 或 CAN2 波特率，上位机在保存成功后会自动请求一次软件复位；随后将对应 CAN 对端切换至相同波特率。
8. 重启 BMS 后再次打开 **Configuration**，确认各字段仍为保存值。

若保存被拒绝，上位机会显示具体原因：

| 原因 | 含义 |
| --- | --- |
| BMS must be INACTIVE… | 当前处于运行状态或 AIR 未断开。 |
| outside the safe range | 数值越界，或上下限之间的关系不合法。 |
| Flash write or verification failed | Flash 写入或读回 CRC 校验失败，旧配置仍保持有效。 |

## 3. 可配置参数

| 定义 | 参数 | 单位 |
| --- | --- | --- |
| AFE数量 | `TOTAL_IC` | - |
| 放电持续时间 | `DISCHARGE_DURATION` | ms |
| 无故障时间阈值 | `NO_ERROR_TIME` | ms |
| 继电器最短连续闭合间隔 | `AIR_CLOSE_WAIT` | ms |
| 最长预充等待时间 | `PRECHARGE_95_WAIT_TIME` | ms |
| 预充保持时间 | `PRE_HOLD_TIME` | ms |
| 最大状态保持时间 | `MAX_STATE_HOLD_TIME` | ms |
| 电机使能信号超时时间 | `DRIVE_MODE_TIMEOUT` | ms |
| 最大充电电压 | `MAX_CHARGE_VOLT` | 0.1 V |
| 最大充电电流 | `MAX_CHARGE_CURR` | 0.1 A |
| 分流器超时时间 | `SHUNT_TIMEOUT` | ms |
| 放电过流阈值 | `SHUNT_OVERCURRENT_DISCHARGE` | mA |
| 充电过流阈值 | `SHUNT_OVERCURRENT_CHARGE` | mA |
| 动能回收过流阈值 | `SHUNT_OVERCURRENT_REGEN` | mA |
| IVT过温阈值 | `SHUNT_OVERTEMP` | 0.1 °C |
| 最大/最小单体电压阈值 | `MAX_CELL_VOLT` / `MIN_CELL_VOLT` | mV |
| 最大/最小温度阈值 | `MAX_CELL_TEMP` / `MIN_CELL_TEMP` | °C |
| PEC异常触发故障电流阈值 | `PEC_CURR_THESHOLD` | mA |
| IVT任意故障延时时间 | 分流器超时、过流、过温、电压不匹配确认时间 | ms |
| AFE任意故障延时时间 | 从机过压、欠压、温度、通信确认时间 | ms |
| CAN1/CAN2 波特率 | `CAN1_BAUDRATE` / `CAN2_BAUDRATE` | 下拉选择：250 kbit/s 或 500 kbit/s |

- 所有参数在底层固件中均有默认值，可在`bms_config.c`中进行修改。

`SHUNT_OVERCURRENT_REGEN` 会被保存并通过协议返回；当前分流器保护路径尚未按回收电流单独分支，因此改变它不会改变现有的过流判定。若后续加入再生工况判别，可直接使用此保存值。

## 4. Flash 保存与掉电行为

配置区位于 STM32F407 **Sector 10**：`0x080C0000..0x080DFFFF`（128 KiB）。APP 链接区缩小为 `0x08010000..0x080BFFFF`，Bootloader 有效性 metadata 仍位于 `0x080FFF00..0x080FFFFF`。

每次保存会向 Sector 10 追加一条记录：

```text
magic | format version | generation | 30 个参数 | CRC32 | commit marker
```

- 只有 CRC32 正确且最后的 `commit marker` 已写入的记录才会在启动时使用。
- 写入中掉电：未提交的新记录无效，启动时继续读取上一条有效记录。
- Sector 写满时才擦除并从头开始记录；若恰好在该擦除阶段掉电，配置可能回退为编译期默认值。
- 写入成功后固件会读回并验证记录；失败不会把候选参数应用到运行状态。

## 5. 固件升级的影响

串口 `Firmware Upgrade` 会按现有 Bootloader 流程擦除 Sector 4–11。因此 **每次固件升级都会清除已保存的配置**，新 APP 首次启动时会采用上述默认值。这是本功能的既定设计。

升级前如需保留数值，请在上位机记录配置；升级后打开 **Configuration** 并重新保存。

## 6. 串口协议

所有帧沿用现有 `B5 4D`、版本号和 CRC16/Modbus 封装。

| 类型 | ID | 负载 | 方向 |
| --- | --- | --- | --- |
| `CONFIGURATION` | `0x04` | 30 个 little-endian `int32` 字段（120 B） | BMS → 上位机 |
| `GET_CONFIGURATION` | `0x15` | 空 | 上位机 → BMS |
| `SET_CONFIGURATION` | `0x16` | 同 `CONFIGURATION`（120 B） | 上位机 → BMS |
| `SOFTWARE_RESET` | `0x18` | 空 | 上位机 → BMS |

`GET_CONFIGURATION` 成功后，BMS 发送 `CONFIGURATION` 再发送 ACK。`SET_CONFIGURATION` 在保存成功后也回传新配置和 ACK。
当且仅当保存内容中的 CAN1 或 CAN2 波特率发生变化时，上位机在收到 `SET_CONFIGURATION` 成功 ACK 后发送 `SOFTWARE_RESET`。

`SET_CONFIGURATION` 的 ACK `reason`：`1` 为状态不安全，`2` 为参数校验失败，`3` 为 Flash 写入或读回校验失败。

## 7. 安全边界

- 所有可修改参数均有范围限制，若需要修改范围需要在底层固件`bms_config.c`中进行修改。
