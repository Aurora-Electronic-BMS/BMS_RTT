# Boot_test - IAP 升级测试工程

> 面向移植：本文档记录完整的 IAP 机制、通信协议、Flash 分区、Boot/APP 交互逻辑和构建方法，方便将 IAP 功能移植到其他 STM32 工程。

## 1. 系统概述

独立的 IAP (In-Application Programming) 升级测试工程，包含 Bootloader 和 APP 两个独立子工程。用于验证串口固件升级机制，可作为 BMS_V1.0 Bootloader 的参考实现。

### 核心功能

- **Bootloader**: 串口固件升级、CRC32 校验、向量表验证、安全跳转
- **APP**: 基于 RT-Thread 的 LED 闪烁 + Boot 触发轮询
- **上位机工具**: Python CLI/GUI 升级工具 + ST-Link 烧录工具

## 2. 目标硬件

| 项目 | 规格 |
|------|------|
| MCU | STM32F407ZGT6 (Cortex-M4F, 64 MHz) |
| Flash | 1024 KB |
| RAM | 128 KB SRAM + 64 KB CCM-RAM |
| 时钟 | HSI 16 MHz → PLL → SYSCLK = 64 MHz |
| 串口 | UART5: PC12 (TX), PD2 (RX), 115200 8N1 |
| 调试 | SWD (ST-Link) |
| USB-TTL | CH340 连接 UART5 (TX/RX 交叉) |

## 3. Flash 存储映射

```
0x08000000 ┌──────────────────────────────┐
           │   Bootloader (64 KB)         │  Sector 0-3
0x08010000 ├──────────────────────────────┤
           │   APP (960 KB)               │  Sector 4-11
0x080FFF00 ├──────────────────────────────┤
           │   Metadata (256 B)           │  Sector 11 末尾
0x08100000 └──────────────────────────────┘
```

**STM32F407 Sector 大小**:

| Sector | 起始地址 | 大小 |
|--------|----------|------|
| 0 | 0x08000000 | 16 KB |
| 1 | 0x08004000 | 16 KB |
| 2 | 0x08008000 | 16 KB |
| 3 | 0x0800C000 | 16 KB |
| 4 | 0x08010000 | 64 KB |
| 5 | 0x08020000 | 128 KB |
| 6 | 0x08040000 | 128 KB |
| 7 | 0x08060000 | 128 KB |
| 8 | 0x08080000 | 128 KB |
| 9 | 0x080A0000 | 128 KB |
| 10 | 0x080C0000 | 128 KB |
| 11 | 0x080E0000 | 128 KB |

## 4. 元数据结构

位于 Flash 末尾 256 字节区域 (实际使用 20 字节):

```c
typedef struct {
    uint32_t magic;          // 0xB00710AD
    uint32_t magic_inverse;  // ~0xB00710AD
    uint32_t image_length;   // APP 镜像长度 (4 字节对齐)
    uint32_t image_crc32;    // CRC32-IEEE 校验
    uint32_t crc_inverse;    // ~image_crc32
} app_metadata_t;            // 共 20 字节
```

**保护机制**: magic 最后写入, 断电时如果 magic 无效, Bootloader 认为 APP 无效, 保持升级模式。

## 5. Boot 序列

```
上电复位
  │
  ├─ 初始化 HAL, GPIO, CRC, UART5
  │
  ├─ 检查 RTC 备份寄存器 (BKP0R == 0xB007B007?)
  │   ├─ 是 → 强制升级模式, 跳到步骤 ③
  │   └─ 否 → 继续
  │
  ├─ 验证 APP 元数据
  │   ├─ 无效 → 跳到步骤 ③ (等待 HELLO)
  │   └─ 有效 → 继续
  │
  ├─ 等待 500ms 检测 HELLO 握手
  │   ├─ 收到 HELLO → 进入升级会话 (步骤 ③)
  │   └─ 未收到 → 跳转到 APP (步骤 ②)
  │
  ├─ ② jump_to_app():
  │   ├─ __disable_irq()
  │   ├─ HAL 外设反初始化 (UART, CRC, HAL, RCC)
  │   ├─ 停止 SysTick, 清除所有中断挂起
  │   ├─ SCB->VTOR = 0x08010000
  │   ├─ 重置为特权 MSP 模式
  │   ├─ DSB + ISB 内存屏障
  │   └─ MSR MSP = APP 栈指针, BX Reset_Handler
  │
  └─ ③ 升级会话:
      ├─ HELLO → ACK, 锁定串口
      ├─ BEGIN → 擦除 Sector 4-11, 记录 image_length + crc32
      ├─ DATA  → 逐字节写入 Flash + 读回校验
      ├─ END   → CRC32 全量校验 + 向量表验证 + 写入元数据
      └─ RUN   → 跳转到 APP (6 秒超时自动跳转)
```

## 6. APP 返回 Bootloader

APP 通过以下步骤触发重新升级:

1. 轮询 UART5 接收 `ENTER_BOOTLOADER` (0x13) 命令帧
2. 验证帧完整性 (CRC16)
3. 发送 ACK 响应
4. 写入 RTC 备份寄存器: `BKP0R = 0xB007B007`, `BKP1R = ~0xB007B007`
5. 调用 `NVIC_SystemReset()` 软件复位
6. Bootloader 检测到备份寄存器标志, 清除标志, 进入升级模式

## 7. 串口升级协议 (B5 4D)

### 帧格式

```
┌──────────┬─────────┬───────┬──────────┬──────────┬─────────────┬──────────┐
│ MAGIC(2) │ VER(1)  │ CMD(1)│ SEQ(2)   │ LEN(2)   │ DATA(0-260) │ CRC16(2) │
│ 0xB5 0x4D│ 0x01    │       │ LE       │ LE       │ N bytes     │ LE       │
└──────────┴─────────┴───────┴──────────┴──────────┴─────────────┴──────────┘
```

- **CRC16-Modbus**: 多项式 0xA001, 初始值 0xFFFF, 计算范围 = VER 到 DATA
- **字节序**: 小端 (Little-Endian)

### 命令表

| CMD | 名称 | 值 | 载荷 | 方向 |
|-----|------|----|------|------|
| HELLO | 握手 | 0x30 | 无 | Host → Boot |
| BEGIN | 开始升级 | 0x31 | image_length(4B) + image_crc32(4B) | Host → Boot |
| DATA | 写入数据 | 0x32 | offset(4B) + data[1..256] | Host → Boot |
| END | 校验完成 | 0x33 | 无 | Host → Boot |
| RUN | 跳转 APP | 0x34 | 无 | Host → Boot |
| ENTER_BOOT | 返回 Boot | 0x13 | 无 | Host → APP |

### 响应格式

相同帧结构, CMD 替换为:
- `0x02` = ACK (成功)
- `0x03` = NACK (失败)

响应载荷 (6 字节): `echo_seq(2B) + original_cmd(1B) + result_code(1B) + expected_next_seq(2B)`

### 结果码

| 码 | 含义 |
|----|------|
| 0x00 | OK |
| 0x01 | CRC 错误 |
| 0x02 | 长度错误 |
| 0x03 | 命令错误 |
| 0x04 | 状态错误 |
| 0x05 | 序列号错误 |
| 0x06 | Flash 写入错误 |
| 0x07 | 镜像 CRC 不匹配 |
| 0x08 | 向量表无效 |

### 序列号机制

- Host 从 0xFFFF 开始, `_next_seq()` 返回 0 作为首个序列号
- Boot 跟踪期望序列号, NACK 帧中返回期望的下一个序列号用于自动重同步

## 8. APP 配置要点

### 链接脚本

APP 的链接脚本必须将 FLASH 起始地址设为 `0x08010000`:

```ld
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08010000, LENGTH = 960K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (rwx): ORIGIN = 0x10000000, LENGTH = 64K
}
```

### APP main() 必须包含

```c
// 1. 重定位中断向量表
SCB->VTOR = FLASH_BASE | 0x10000U;  // 0x08010000

// 2. 设置中断优先级分组
HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

// 3. 重新使能中断 (Bootloader 跳转前已关闭)
__enable_irq();
```

### SOC 估算（OCV 查表 + 安时积分）

`Core/Src/soc_estimation.c`：静置（|电流| < 200mA 持续 100s）时用最低单体电压
`min_voltage` 查 OCV-SOC 表定基准；有电流时对分流器累计电量
`shunt_data.current_counter` 做安时积分。符号约定与分流器一致（**充电为正、放电为负**），
输出 `current_soc` 钳位到 [0,100]。

接入点（控制线程 150ms 重活分支，持 `bms_state_lock`）：`bms_app_start()` 调 `soc_init()`；
heavy 分支 `shunt_check()` 之后调 `soc_update()`。消费端三处：CAN STATUS1 的 `data1[1]`、
遥测固定区中的 1 字节、串口 `[ST] ... soc=`。

> 注意：BMS_V1.0 裸机版同名模块原先**零调用**，且算法有方向/符号缺陷（只认充电方向、
> 积分符号反、无钳位）。已同步修正算法（仍未接入其主循环）。两版算法由
> `tools/soc_test` 用同一套断言跑真实的 `soc_estimation.c` 验证。

## 9. 构建方法

### 环境要求

- CMake >= 3.22
- Ninja
- arm-none-eabi-gcc
- Python 3 + pyserial (上位机工具)

### 构建 Bootloader

```bash
cd Bootloader_test
cmake --preset Debug
cmake --build --preset Debug
```

输出: `build/Debug/Bootloader_test.elf` / `Bootloader_test.bin`

### 构建 APP

```bash
cd APP_test
cmake --preset Debug
cmake --build --preset Debug
```

输出: `build/Debug/APP_test.elf` / `APP_test.bin`

### 烧录方式

**方式 1: ST-Link (首次烧录)**
```bash
# 先烧 Bootloader (0x08000000)
# 再烧 APP (0x08010000)
# 使用 STM32CubeIDE 或 STM32_Programmer_CLI
```

**方式 2: 串口升级 (后续更新)**
```bash
python tools/iap_host_bms.py <COM端口> <APP_test.bin路径>
```

## 10. 上位机工具

| 工具 | 协议 | 用途 |
|------|------|------|
| `iap_host_bms.py` | B5 4D | CLI 升级工具 (当前版本) |
| `iap_host_bms_gui.py` | B5 4D | Tkinter GUI 升级 + ST-Link 烧录 |
| `flash.py` / `flash.bat` | B5 4D + OpenOCD | 组合工具 (ST-Link 烧 Bootloader, IAP 烧 APP) |
| `write_metadata.py` | STM32_Programmer | 独立元数据写入工具 |

## 11. 调试配置

### STM32CubeIDE 调试 APP

1. Debug 配置中设置向量表偏移: `SCB->VTOR = 0x08010000`
2. 加载 `Bootloader_test.elf` 符号文件以解析 Bootloader 地址空间
3. 断点可能显示在 `0x080012E8` 地址, 这是正常的 (加载 Bootloader ELF 即可解决)

### 调试注意事项

- APP 调试时, Bootloader 必须已烧录且有效, 否则无法启动
- 如果不加载 Bootloader 符号, 断点在 Bootloader 空间会显示无调试信息
- RT-Thread 的 SysTick 中断在 APP 中运行, HAL 使用 TIM7 作为时基

## 12. 移植指南

### 移植到其他 STM32 芯片

1. **修改 Flash 扇区布局**: 不同芯片的 Sector 大小和数量不同
2. **更新链接脚本**: Bootloader 和 APP 的 FLASH 起始地址和大小
3. **调整元数据位置**: 确保在 Flash 末尾, 不与其他数据冲突
4. **修改 UART 引脚**: 更新 GPIO 和 UART 外设配置
5. **验证 CRC 算法**: 确保硬件 CRC 或软件 CRC 与上位机一致
6. **调整 RTC 备份寄存器**: 不同芯片的备份寄存器数量和地址不同

### 单独移植各组件

| 组件 | 依赖 | 移植步骤 |
|------|------|----------|
| Bootloader 主逻辑 | Flash, CRC, UART | 修改 Flash 扇区地址, 调整 HAL 驱动 |
| 跳转机制 | NVIC, SCB | 检查目标芯片的 VTOR/SCB 寄存器 |
| 帧协议解析 | 无 | 直接复制, 修改帧大小限制 |
| 元数据校验 | Flash | 修改元数据存储地址 |
| APP 触发返回 | RTC BKP | 确认目标芯片的备份寄存器机制 |
| 上位机工具 | pyserial | 修改默认串口和波特率 |

### 关键注意事项

- **Flash 编程方式**: 当前使用逐字节编程 (`FLASH_TYPEPROGRAM_BYTE`) + 读回校验, 兼容性最好
- **Sector 擦除顺序**: BEGIN 时擦除 Sector 4-11, 注意不同芯片的扇区大小
- **向量表偏移**: APP 必须设置 `SCB->VTOR`, 否则中断会跳转到错误地址
- **中断使能**: APP 启动后必须调用 `__enable_irq()`, 否则所有中断不工作
- **SysTick 冲突**: 如果 APP 使用 RTOS, SysTick 归 RTOS 管理, HAL 需要其他时基
- **断电保护**: 元数据最后写入, 确保升级过程中断电不会导致无效 APP 被执行
- **序列号同步**: NACK 帧中的 `expected_next_seq` 用于 Host 自动重同步, 无需重发 HELLO

### 移植到 FDCAN 芯片 (如 STM32H7)

如果需要将升级通道从 UART 改为 CAN-FD:
1. 将帧协议适配到 CAN-FD 帧 (最大 64 字节载荷)
2. 使用 CAN-FD 的 BRS (Bit Rate Switch) 提高速度
3. 调整滤波器配置
4. 注意: 当前协议的 256 字节 DATA 载荷需要拆分为多个 CAN-FD 帧

## 13. 文件结构

```
Boot_test/
├── Bootloader_test/                 # Bootloader 子工程
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── STM32F407ZGTX_FLASH.ld       # 链接脚本 (@ 0x08000000)
│   ├── Core/
│   │   ├── Inc/                     # main.h, gpio.h, usart.h, crc.h
│   │   └── Src/
│   │       ├── main.c               # ★ 完整 Bootloader (980 行)
│   │       ├── gpio.c, usart.c, crc.c
│   │       └── stm32f4xx_it.c
│   ├── Drivers/                     # STM32 HAL + CMSIS
│   └── cmake/stm32cubemx/          # CubeMX 自动生成的构建规则
│
├── APP_test/                        # APP 子工程
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── STM32F407ZGTX_FLASH.ld       # 链接脚本 (@ 0x08010000)
│   ├── Core/
│   │   ├── Inc/
│   │   │   ├── main.h               # LED0=PE3, LED1=PE4
│   │   │   └── RTE_Components.h     # RT-Thread 组件使能
│   │   └── Src/
│   │       ├── main.c               # ★ APP 逻辑: LED + Boot 触发
│   │       └── stm32f4xx_hal_timebase_tim.c  # TIM7 HAL 时基
│   ├── Middlewares/Third_Party/RealThread_RTOS_RT-Thread/  # RT-Thread 3.1.3
│   └── Drivers/
│
├── tools/                           # 上位机工具
│   ├── iap_host_bms.py             # CLI 升级工具
│   ├── iap_host_bms_gui.py         # GUI 升级工具
│   ├── flash.py / flash.bat        # 组合烧录工具
│   ├── write_metadata.py           # 元数据写入
│   ├── bms_logic_test.py           # 状态机端到端逻辑验证
│   ├── bms_telemetry_test.py       # 遥测协议验证
│   ├── bms_fault_inject.py         # 故障注入
│   ├── bms_soak.py                 # 长跑/浸泡测试
│   ├── fsm_test/                   # 状态机纯内核宿主机单测
│   ├── ring_test/                  # SPSC 环形缓冲宿主机并发测试
│   └── soc_test/                   # SOC 估算宿主机单测 (RT/裸机两版同跑)
│
├── CubeIDE配置与调试IAP指南.md       # CubeIDE IAP 配置指南
├── IAP升级从零配置指南.md            # 从零搭建 IAP 的完整教程
└── doces/                           # HTML 文档页面
```
