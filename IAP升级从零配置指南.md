# STM32 IAP 固件升级：从零配置完整指南

> 本文基于 `d:\dkt_stm32\Boot_test` 工程（STM32F407ZGT6）的完整开发与真机调试实战总结。
> 目标是让一个刚用 CubeMX 生成的空工程，一步步变成可用的串口 IAP 升级程序。
> 所有"坑"都来自真实踩坑记录，按**现象 → 原因 → 解决**讲清楚。

---

## 0. 原理：IAP 到底是什么

**IAP（In-Application Programming）** = 芯片上电先运行一个小程序（**bootloader**），由它决定：
- 收到上位机的升级指令 → 通过串口接收新固件，**擦写 Flash**，然后跳转到新程序；
- 没有升级指令 → 直接把控制权交给已有的 **APP**。

Flash 里同时住着两个独立编译的程序：

```
0x08000000 ┌─────────────────┐
           │   Bootloader    │  ← 上电先跑它（链接在 0x08000000）
0x08010000 ├─────────────────┤
           │      APP        │  ← 真正的业务程序（链接在 0x08010000）
0x08100000 └─────────────────┘
```

**最核心的机制**（不理解这个，后面全白搭）：

APP 被链接到非 0 地址后，它的**中断向量表**也搬到了 0x08010000。而 Cortex-M 硬件取中断向量永远是从 `SCB->VTOR + 4×中断号` 读的。所以跳转前必须：
1. **`SCB->VTOR = 0x08010000`** —— 让中断从 APP 的向量表取（不做这一步，一开中断就 HardFault / 跑飞）；
2. **`__set_MSP(向量表第1个字)`** —— 换栈，第1个字是 APP 的栈顶 `_estack`；
3. **函数指针跳 `向量表第2个字`（Reset_Handler）** —— 让它完整执行 SystemInit → 初始化 .data/.bss → main。

**一句话：跳转 = 关中断 → 停 SysTick → 改 VTOR → 换 MSP → 跳 Reset_Handler。**

---

## 1. 硬件准备

| 器件 | 说明 |
|------|------|
| STM32F407ZGT6 开发板 | 本例 1MB Flash / 192KB RAM，8MHz 外部晶振 |
| ST-Link | 调试 + 首次烧录（SWD：SWDIO/PA13，SWCLK/PA14，GND） |
| USB 转 TTL（本例 CH340） | 串口升级通道 |

**串口接线（最常见的大坑！）** 本例 APP/Bootloader 都用 **UART5**（PC12=TX，PD2=RX）：

```
MCU PC12 (UART5_TX) ──────── CH340 的 RX
MCU PD2  (UART5_RX) ──────── CH340 的 TX
MCU GND  ─────────────────── CH340 的 GND
```

⚠️ **必须交叉接**：MCU 的 TX 接 USB 模块的 RX，MCU 的 RX 接 USB 模块的 TX。
判断方法：让 MCU 发一个字节，上位机能收到 → 这条方向通；再让上位机发 `0x00`，用万用表量 MCU RX 引脚能被拉低 → 另一条方向通。**只通一条方向 = 有一根线没接/插错位。**

> 本例实测：CH340 RX←PC12 通、CH340 TX→PD2 一开始没信号，握手一直失败，接好后立刻通。

---

## 2. 软件准备

| 工具 | 用途 |
|------|------|
| STM32CubeMX | 生成两个工程的初始化代码 |
| STM32CubeCLT（或 CubeIDE） | 含 `arm-none-eabi-gcc`、`cmake`、`ninja`、`STM32_Programmer_CLI` |
| Python3 + pyserial | 跑上位机升级脚本 |

验证工具链：
```bash
arm-none-eabi-gcc --version
cmake --version
STM32_Programmer_CLI -l stlink    # 能列出 ST-Link 就说明烧录环境 OK
```

---

## 3. Flash 分区规划

STM32F407 的扇区结构（1MB，Bank1）：

| 扇区 | 起始地址 | 大小 |
|------|---------|------|
| Sector 0 | 0x08000000 | 16KB |
| Sector 1 | 0x08004000 | 16KB |
| Sector 2 | 0x08008000 | 16KB |
| Sector 3 | 0x0800C000 | 16KB |
| Sector 4 | 0x08010000 | 64KB |
| Sector 5~11 | 0x08020000 | 128KB × 7 |

**分区方案（本例）**：
- **Bootloader**：`0x08000000`，占 Sector 0~3（64KB），链接脚本用默认值不用改；
- **APP**：`0x08010000`，占 Sector 4~11（960KB），链接脚本要改；
- **元数据**：`0x080FFF00` 起保留 256B；升级标志位于 `0x080FFE00`，均位于 Sector 11；
- 升级时只擦写 APP 所在扇区（Sector 2 起），bootloader 自保留。

---

## 4. Bootloader 工程配置

### 4.1 CubeMX 配置
1. 新建工程，选 STM32F407ZGTx。
2. **RCC**：HSE 外部晶振，时钟树配到 168MHz（PLL M=4/N=168/P=2）。
3. **UART5**：Asynchronous（PC12=TX、PD2=RX），115200-8-N-1。
4. **CRC**：勾选使能（用于整包校验）。
5. SYS：Debug 选 Serial Wire。
6. Project Manager → Toolchain 选 **CMake**，生成。

> 端口/外设按你的板子改，本文以 UART5 + CRC 为例。

### 4.2 链接脚本
Bootloader 链接在 `0x08000000`（CubeMX 默认），**不用改**。

### 4.3 核心 IAP 代码（iap.c / iap.h）

**① 串口帧协议**（当前 `Bootloader_test/Core/Src/iap.c` 实现）：

```
帧头2字节 | 版本1字节 | 类型1字节 | 序号2字节 | 长度2字节 | 数据N字节 | CRC16 2字节
 0xB5 0x4D | VERSION |   TYPE   |  SEQ(LE) | LEN(LE)  |   DATA   | CRC16-Modbus(LE)
```
`SUM = (CMD + 长度字节 + 所有数据字节) 累加和取低8位`。

| 命令 | 值 | 数据载荷 | 应答 |
|------|----|---------|------|
| HELLO | 0x30 | 无 | ACK/NACK |
| BEGIN | 0x31 | 镜像长度(4B)+CRC32(4B，小端) | ACK/NACK |
| DATA | 0x32 | offset(4B，小端)+数据 | ACK/NACK |
| END | 0x33 | 无 | ACK/NACK |
| RUN | 0x34 | 无 | ACK 后跳转 |

应答规则：应答命令 = 请求命令 | 0x80；载荷首字节 0x00=成功、0x01=失败。

**② 帧接收状态机**（轮询逐字节解析）：

```c
// 状态：WAIT_HDR1 → WAIT_HDR2 → CMD → LEN_L → LEN_H → DATA → SUM
// 收满一帧且校验和通过 → rx_ready = 1，主循环里分发
```

> ⚠️ **大坑 1（真机必炸）**：帧校验通过后调用 `iap_rx_reset()` 时，**不要清 `rx_len`**！
> 否则命令处理函数（如 `iap_handle_write`）读到的 `rx_len` 永远是 0，所有命令都失败。
> 因为 `iap_rx_reset()` 在帧处理前就被调用了。正确做法：reset 只复位状态机和 `rx_idx`，`rx_len` 留给命令处理函数读，下一帧的 LEN 字段会重新覆盖。

**③ Flash 擦除与写入**：

```c
HAL_FLASH_Unlock();
// 擦除：HAL_FLASHEx_Erase(&er, &err)，er.Sector 从 4 到 11
// 写入：逐 4 字节调用 HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, val)
HAL_FLASH_Lock();
// 全程 __disable_irq() / 完成后 __enable_irq()
```

> ⚠️ **大坑 2（克隆片/部分 F407 必炸）**：**不要用 `FLASH_TYPEPROGRAM_DOUBLEWORD`（64位双字编程）**！
> 实测这颗芯片双字编程报 `PGAERR + PGSERR`，改用 **`FLASH_TYPEPROGRAM_WORD`（32位）** + **4 字节对齐** 就一切正常。ST-Link 烧录也是用字编程，所以调试器能烧、固件自己双字烧不了。地址都按 4 字节对齐，最后不足 4 字节用 0xFF 补齐。

**④ 整包 CRC32**（用硬件 CRC 外设）：

```c
static uint32_t iap_calc_crc32(uint32_t base, uint32_t size)
{
  volatile uint32_t *p = (volatile uint32_t *)base;
  __HAL_CRC_DR_RESET(&hcrc);
  uint32_t crc = 0;
  for (uint32_t i = 0; i < size/4; i++)
    crc = HAL_CRC_Accumulate(&hcrc, (uint32_t*)&p[i], 1);  // 逐字喂入，返回当前CRC
  return crc;
}
```

当前 Bootloader 使用标准 IEEE CRC32，Python 上位机使用 `zlib.crc32()`；镜像长度先按 4 字节边界用 `0xFF` 补齐。

**⑤ 跳转 APP**（整个 IAP 的核心）：

```c
void iap_jump_to_app(void)
{
  typedef void (*pFunction)(void);
  if (!iap_app_valid()) return;              // 校验向量表：SP 在 RAM、PC 在 APP 区

  __disable_irq();                            // ① 关全局中断
  SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;  // ② 停 SysTick
  SCB->VTOR = APP_BASE_ADDR;                  // ③ 重定位向量表
  __set_MSP(*(volatile uint32_t*)APP_BASE_ADDR);          // ④ 换栈
  ((pFunction)*(volatile uint32_t*)(APP_BASE_ADDR + 4))(); // ⑤ 跳 Reset_Handler
}
```

**⑥ main.c 主流程**：

```c
int main(void)
{
  HAL_Init();  SystemClock_Config();
  MX_GPIO_Init();  MX_CRC_Init();  MX_UART5_Init();

  iap_init();
  if (!iap_wait_handshake(2000) && iap_app_valid())
    iap_jump_to_app();            // 没握手且有有效APP → 直接启动APP
  iap_update_loop();              // 否则进升级模式（收到RUN后内部跳转，不返回）
}
```

流程：上电 → 监听串口握手窗口 → 收到 HELLO 进升级；否则有有效 APP 就跳转，没有就留在升级模式等命令。

---

## 5. APP 工程配置

### 5.1 CubeMX 配置
1. 新建工程，选同型号芯片。
2. **RCC**：和 bootloader 完全一样的 168MHz 时钟。
3. **UART5**：同 bootloader（PC12/PD2，115200）。
4. **GPIO**：LED0=PE3、LED1=PE4，输出推挽（用于观察 APP 是否在跑）。
5. 生成 **CMake** 工程。

### 5.2 链接脚本 —— 关键一步
把 `STM32F407xx_FLASH.ld` 里 FLASH 区从默认改为 APP 分区地址：

```
FLASH (rx) : ORIGIN = 0x08010000, LENGTH = 0xEF000   /* 960KB */
```

不改这行，APP 会链接到 0x08000000 和 bootloader 重叠，跳转目标就是错的。

### 5.3 main.c —— 两个必须做的事

```c
int main(void)
{
  /* USER CODE BEGIN 1 */
  SCB->VTOR = FLASH_BASE | 0x10000U;         // ① 重定位向量表
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  __enable_irq();                             // ② 重新使能中断（关键！）
  /* USER CODE END 1 */
  HAL_Init();  SystemClock_Config();
  MX_GPIO_Init();  MX_UART5_Init();
  ...
}
```

> ⚠️ **大坑 4（真机必炸）**：bootloader 跳转前 `__disable_irq()` 关了全局中断，
> **APP 必须在 main 开头 `__enable_irq()`**，否则 SysTick 中断永不触发、`HAL_GetTick()` 冻结、
> 所有延时/定时全失效。真机症状：**LED 常亮不翻转、`uwTick` 一直为 0**。
> （UART 轮询不受影响，所以升级流程还能跑，很容易漏掉这个 bug。）

### 5.4 主循环 —— 测试代码 + 可回 bootloader

```c
while (1)
{
  // 连续轮询 UART5：收到合法升级触发帧 → 软复位回 bootloader
  if (__HAL_UART_GET_FLAG(&huart5, UART_FLAG_RXNE))
  {
    uint8_t b = huart5.Instance->DR & 0xFF;
    // upgrade_trigger.c 校验 AA55、命令、参数和 CRC16-CCITT 后调用 NVIC_SystemReset();
  }
  // 每 250ms 翻转 LED0/LED1（证明 APP 在跑）
  if (HAL_GetTick() - last_tick >= 250) { HAL_GPIO_TogglePin(...); }
}
```

**升级触发帧**：APP 校验 `0xAA55`、命令 `0x0001`、参数和 CRC16-CCITT 后写入升级标志并软件复位回 Bootloader。

---

## 6. 上位机脚本（tools/iap_host_bms.py）

功能：读 .bin → 补 0xFF 到 4 字节对齐 → 算 CRC32 → HELLO→BEGIN→DATA→END→RUN。

**关键：CRC 算法必须和 Bootloader 一致**：

```python
import zlib

def crc32_zlib(data):
  return zlib.crc32(data) & 0xFFFFFFFF
```

**DATA 帧**：当前协议最大 payload 为 256B，包含 4B offset，因此单帧数据最多 252B。
注意：当前 `tools/iap_host_bms.py` 和 GUI 代码中的 `chunk_size` 仍为 256B，实际发送时会产生 260B payload；
正式升级前应将其改为不超过 252B，或同步放宽 Bootloader 的 `BOOT_MAX_PAYLOAD`。

**连接兼容**：设备可能正在跑 APP，GUI 首次 HELLO 无响应时发送 APP 升级触发帧，等待软件复位后重试 HELLO。

---

## 7. 烧录与测试

### 7.1 首次烧录（ST-Link）
```bash
# 烧 bootloader 到 0x08000000
STM32_Programmer_CLI -c port=SWD mode=UR reset=HWrst \
    -w Bootloader_test/build/Debug/Bootloader_test.bin 0x08000000 -v
# 烧 APP 到 0x08010000
STM32_Programmer_CLI -c port=SWD mode=UR reset=HWrst \
  -w APP_test/build/Debug/APP_test.bin 0x08010000 -v
```

### 7.2 正常启动验证
复位板子 → 2 秒内不发握手 → bootloader 跳转 APP → **LED 每 250ms 交替闪烁** = 成功。

### 7.3 串口升级验证
```bash
python tools/iap_host_bms.py --port COM7 --bin APP_test/build/Debug/APP_test.bin
```
应看到：连接 → 擦除 → 写入 → CRC 校验通过 → 跳转。升级后 LED 若变化（比如改过代码），说明新 APP 生效。

### 7.4 随时回 bootloader
上位机发送升级触发帧 → APP 校验后软复位 → Bootloader 等待 HELLO → 可再次升级。

---

## 8. 坑清单总表（按出现顺序）

| # | 现象 | 原因 | 解决 |
|---|------|------|------|
| 1 | 串口握手一直失败 | **接线只通了一条方向**（TX/RX 没交叉或线松） | 交叉接线；用"MCU 发字节能收到 / 发0x00能拉低"双向验证 |
| 2 | 工具探测不到 ST-Link/COM口 | USB 驱动未装好 | 重插/装驱动 |
| 3 | 升级脚本报"数据超过256字节" | DATA payload 上限为 256B，包含 4B offset | 单帧实际数据最多 252B |
| 4 | BEGIN/DATA/END 全失败 | `iap_rx_reset()` 把 `rx_len` 清了，命令读到长度0 | reset 不清 `rx_len` |
| 5 | DATA 写入失败 | 地址、长度或 Flash 范围不符合当前会话 | 检查 offset 连续性、镜像长度和 APP 分区 |
| 6 | 整包 CRC 校验不过 | 镜像补齐方式或 CRC 算法不一致 | 使用 `zlib.crc32()`，并按 4B 用 `0xFF` 补齐 |
| 7 | LED 常亮不翻转 | **APP 没 `__enable_irq()`**，SysTick 冻结 | APP main 开头 `__enable_irq()` |
| 8 | APP 被判无效跳不了 | 栈顶 `0x20020000` 被 `sp<0x20020000` 误杀 | 校验范围放宽到 `0x20030000` |
| 9 | gdbserver 占用 ST-Link | 调试服务器一直持有 ST-Link | 用 STM32_Programmer 前杀进程 |
| 10 | 构建不出 .bin | CubeMX 默认只出 .elf | CMake 加 objcopy 后置命令 |

---

## 9. 协议速查表（帧格式）

```
帧： B5 4D | VERSION | TYPE | SEQ_L SEQ_H | LEN_L LEN_H | PAYLOAD | CRC16_L CRC16_H
CRC：对 VERSION 到 PAYLOAD 的全部字节计算 CRC16-Modbus
应答：TYPE 为 ACK(0x02) 或 NACK(0x03)，payload[2] 回显命令，payload[3] 为结果码
```

| CMD | 含义 | 载荷（小端） | 应答数据 |
|-----|------|-------------|---------|
| 0x30 HELLO | 握手 | — | ACK/NACK |
| 0x31 BEGIN | 擦除 APP 区 | 镜像长度(4B)+CRC32(4B) | ACK/NACK |
| 0x32 DATA | 写数据 | offset(4B)+数据 | ACK/NACK |
| 0x33 END | 校验并写元数据 | — | ACK/NACK |
| 0x34 RUN | 跳转 APP | — | ACK 后跳转 |

---

## 10. 参考（本项目可直接复用）

| 文件 | 内容 |
|------|------|
| `Bootloader_test/Core/Src/iap.c` | 协议解析、Flash擦写、CRC、跳转（完整可运行） |
| `Bootloader_test/Core/Src/main.c` | bootloader 主流程（握手→跳转/升级） |
| `APP_test/Core/Src/main.c` | APP：VTOR + `__enable_irq` + LED + 升级触发 |
| `APP_test/STM32F407ZGTX_FLASH.ld` | APP 链接脚本（0x08010000） |
| `tools/iap_host_bms.py` | 当前 B5 4D IAP 上位机脚本 |
| `tools/iap_host_bms_gui.py` | 当前 Tkinter GUI 上位机 |
