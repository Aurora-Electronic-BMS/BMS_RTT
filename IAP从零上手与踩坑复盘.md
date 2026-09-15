# Boot_test IAP：从零上手步骤 + 踩坑复盘

> 面向 STM32F407ZGT6，基于 `D:/dkt_stm32/DKT/Boot_test` 工程的真机调试总结。
> 分为两部分：**A. 为什么之前排查了这么久（现象→原因→解决）**；**B. 从零开始的完整步骤**。
> 本文与实际代码一致（串口已是 **USART6**，不再是旧文档里的 UART5）。

---

# A. 为什么之前搞了这么久 —— 复盘

| # | 现象 | 真正原因 | 解决 |
|---|------|----------|------|
| **1** | 上电无任何串口输出，像"死机"；OpenOCD 看到 `pc=0xfffffffe msp=0xfffffffc`，Handler HardFault | **`0x08000000` 被擦除了（全 `0xFFFFFFFF`），即没有 bootloader**。芯片上电从 `0x08000000` 取复位向量，读到垃圾值，立刻 HardFault。APP 虽然烧在 `0x08010000`，但**永远不会被执行到** | 先把 bootloader 烧回 `0x08000000`，再烧 APP |
| **2** | 一度被当成"硬件问题/线没接好/CH340 问题" | 没有先用调试器定位 **CPU 到底跑到哪（PC 值）** 就靠猜；而且串口还看错了（UART5 vs USART6） | **先读 PC / 寄存器，再下结论**。PC 落在哪一区间就能立刻区分是 bootloader、APP、还是 HardFault |
| **3** | 白改了 UART5 的代码 | 代码/README 写的是 UART5，但**实际接线是 USART6（PC6/PC7）**。方向选错，改再多也没用 | console 与 IAP 统一到 **USART6**（PC6/PC7） |
| **4** | 查 USART 控制寄存器查错 | STM32F4 的 USART 寄存器偏移是 **SR@0x00、DR@0x04、BRR@0x08、CR1@0x0C**。把 `0x40011400`（其实是 SR）当成了 CR1，差点误判"没使能" | 记住偏移；USART 空闲时 **SR=0xC0**（TXE\|TC），CR1 才是 `0x200C`（UE\|TE\|RE） |
| **5** | APP_test 能烧进去、但**不打字、LED 也不闪** | `main()` 是空的 `while(1){}`，而主线程优先级 `RT_MAIN_THREAD_PRIORITY=32/3=10` **高于** LED 线程的 15 → 主线程死循环不让出 CPU，**LED 线程被饿死** | `main()` 里加 `rt_thread_mdelay(1000)` 让出调度（RTT1 的做法是阻塞在信号量上，更干净） |
| **6** | IAP 升级后内容不对 | 构建**只生成 `.elf`，不会自动生成 `.bin`**；IAP 用的是残留的旧 `.bin` | 升级前先 `objcopy` 生成新 `.bin` |
| **7** | 各种"环境噪音" | 串口被占用（`PermissionError`）、残留 `openocd.exe` 占着 ST-Link（`LIBUSB_ERROR_ACCESS`）、Flash 编程中途超时 | `taskkill //F //IM openocd.exe`；关掉串口助手再让脚本独占 COM；失败就重烧一次 |

## 一句话根因

> **APP 链接在 `0x08010000` 时，`0x08000000` 必须存在一个有效的 bootloader；否则芯片在启动阶段就挂了，APP 根本没机会跑。**
>
> 排查"没输出"的第一原则：**别猜硬件，先读 PC。**

## 缺口（设计层面）—— APP_test 已补上 0x13 重入

- **APP_test 现已实现"进入 bootloader"的触发代码（2026-09-11）**：USART6 `RXNE` 中断收到 `0x13` 帧（`B5 4D 01 13 00 00 00 00 <CRC16>`）且 CRC 校验通过后，写 `RTC->BKP0R=0xB007B007`、`BKP1R=~magic`，随即 `NVIC_SystemReset()`；bootloader 的 `consume_boot_request()` 读到标志后常驻 IAP 等待模式。
- 于是上位机可"点一下升级"：`iap_host_bms.py` HELLO 无应答 → 自动发 0x13 触发帧 → APP 软复位进 bootloader → 重试 HELLO 成功（已实测通过）。
- **RTT1 仍未实现该触发**，只能靠下面方式进 bootloader：① 复位后 500ms 窗口内抢发 HELLO；② 写 RTC 备份寄存器强制标志（见 B.6）；③ 若跑的是 APP_test，直接跑上位机即可自动触发。

---

# B. 从零开始的步骤

## B.1 环境与工具

| 工具 | 用途 |
|------|------|
| `arm-none-eabi-gcc` / `objcopy` | 编译、生成 bin |
| CMake ≥ 3.22 + Ninja | 构建 |
| Python 3 + `pyserial` | 上位机升级脚本 |
| OpenOCD（含 `interface/stlink.cfg`、`target/stm32f4x.cfg`） | 烧录 / 调试 |
| ST-Link V2 | SWD 烧录 |
| CH340 USB-TTL | 串口升级通道 |

## B.2 接线

**ST-Link（SWD）**：`SWDIO=PA13`、`SWCLK=PA14`、`GND`、`3V3`。

**CH340 ↔ USART6（必须交叉接！）**：

```
MCU PC6 (USART6_TX) ──────── CH340 的 RX
MCU PC7 (USART6_RX) ──────── CH340 的 TX
MCU GND             ──────── CH340 的 GND
```

- 串口参数：**115200 8N1**，PC 侧端口本例为 **COM7**。
- 只通一个方向 = 有一根线没接/插错位。

## B.3 Flash 分区表

```
0x08000000 ┌───────────────┐
           │ Bootloader    │  Sector 0-3 (64KB)
0x08010000 ├───────────────┤
           │ APP           │  Sector 4-11 (960KB)
0x080FFF00 ├───────────────┤
           │ Metadata(256B)│  Sector 11 末尾
0x08100000 └───────────────┘
```

- APP 起始 `0x08010000`，元数据 `0x080FFF00`。
- 元数据结构：`magic(0xB00710AD) + ~magic + image_length + image_crc32 + ~image_crc32`，**magic 最后写**（掉电安全）。

## B.4 编译

```bash
# Bootloader（链接在 0x08000000）
cd d:/dkt_stm32/DKT/Boot_test/Bootloader_test
cmake --preset Debug && cmake --build --preset Debug
# 产物: build/Debug/Bootloader_test.elf

# APP（本例用 APP_test；链接在 0x08010000）
cd d:/dkt_stm32/DKT/Boot_test/APP_test
cmake --build build/Debug
# ★ 必须手动生成 .bin（构建不自动出）
arm-none-eabi-objcopy -O binary build/Debug/APP_test.elf build/Debug/APP_test.bin
```

## B.5 首次烧录（ST-Link，两个都要烧）

```bash
# ① 先烧 Bootloader 到 0x08000000
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "init; reset halt" \
  -c "program d:/dkt_stm32/DKT/Boot_test/Bootloader_test/build/Debug/Bootloader_test.elf verify" \
  -c "exit"

# ② 再烧 APP 到 0x08010000
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "init; reset halt" \
  -c "program d:/dkt_stm32/DKT/Boot_test/APP_test/build/Debug/APP_test.elf verify" \
  -c "reset run" -c "exit"
```

> ⚠️ **顺序很重要，且两步都要做**。只烧 APP 不烧 bootloader → 上电 HardFault（复盘 #1）。

## B.6 验证与 IAP 升级

**验证**：串口助手连 COM7 @115200，应看到 APP 输出（如 `[APP] led running, tick=...`）。
若没有，先别猜硬件，用调试器读 PC：

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "init; halt" -c "reg pc" -c "resume; exit"
# PC 在 0x0800xxxx → bootloader；0x0801xxxx → APP；0xfffffffe → HardFault（多半 0x08000000 没程序）
```

**进入 bootloader 的三种方式**：

- 方式一（抢窗口）：复位后 **500ms 内**立刻发 HELLO（不稳定，仅应急）。
- **方式二（推荐，稳定）**：复位前写 RTC 强制升级标志：

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "init; reset halt" \
  -c "mww 0x40023840 0x10000000" \
  -c "mww 0x40007000 0x00000100" \
  -c "mww 0x40002850 0xB007B007" \
  -c "mww 0x40002854 0x4FF84FF8" \
  -c "resume; exit"
# 之后 bootloader 会一直停在 IAP 等待模式（不会 500ms 后跳 APP）
```

- **方式三（APP_test 已实现，最省事）**：直接跑 `iap_host_bms.py`。HELLO 无应答时它会自动发 `0x13` 触发帧，APP（APP_test）收到后写 BKP 标志并软复位，即进入 bootloader 升级。

**执行升级**：

```bash
cd d:/dkt_stm32/DKT/Boot_test/tools
python iap_host_bms.py --port COM7 --bin d:/dkt_stm32/DKT/Boot_test/APP_test/build/Debug/APP_test.bin
# 流程: HELLO → BEGIN(擦除 4-11 扇区) → DATA(分包写入) → END(CRC32 校验+写元数据) → RUN(跳转)
# 成功输出: [OK] Upgrade successful!
```

## B.7 通信协议速查（帧格式 `B5 4D`）

```
MAGIC(2) | VER(1) | CMD(1) | SEQ(2 LE) | LEN(2 LE) | DATA(0-256) | CRC16-Modbus(2 LE)
```

| CMD | 值 | 方向 | 说明 |
|-----|----|------|------|
| HELLO | 0x30 | Host→Boot | 握手 |
| BEGIN | 0x31 | Host→Boot | 载荷 = length(4B)+crc32(4B)，触发擦除 |
| DATA | 0x32 | Host→Boot | 载荷 = offset(4B)+data |
| END | 0x33 | Host→Boot | 全量 CRC32 校验 + 写元数据 |
| RUN | 0x34 | Host→Boot | 跳转 APP |
| ENTER_BOOT | 0x13 | Host→**APP** | 要求 APP 重入 bootloader（**APP_test 已实现**；RTT1 未实现） |

应答：ACK=`0x02`、NACK=`0x03`；载荷 = `echo_seq(2B)+cmd(1B)+result(1B)+expected_seq(2B)`。
校验：帧 CRC16-Modbus（poly `0xA001`，init `0xFFFF`）；镜像 CRC32-IEEE（同 `zlib.crc32`）。

## B.8 常用寄存器速查（STM32F4）

| 外设/寄存器 | 地址 | 备注 |
|-------------|------|------|
| USART6 基址 | `0x40011400` | SR@0x00, DR@0x04, BRR@0x08, **CR1@0x0C** |
| USART6 空闲 SR | — | `0xC0`（TXE\|TC） |
| USART6 正常 CR1 | — | `0x200C`（UE\|TE\|RE） |
| USART6 BRR | — | bootloader/APP_test(64MHz, APB2=32MHz) = `0x116`；RTT1(168MHz, APB2=84MHz) = `0x2D9` |
| GPIOC MODER/AFR | `0x40020800` / `0x40020820` | PC6/PC7 = AF8 → MODER 含 `0xA000`，AFR[0]=`0x88000000` |
| RTC BKP0R/BKP1R | `0x40002850` / `0x40002854` | 强制升级标志 |
| PWR_CR | `0x40007000` | bit8=DBP 需置 1 才能写 BKP |
| RCC APB1ENR | `0x40023840` | bit28=PWREN（写 PWR_CR 前先开） |

---

## C. 高频坑速查（一句话版）

1. **只烧 APP 不烧 bootloader → 上电 HardFault。** 两个都要烧，bootloader 在 `0x08000000`。
2. **没输出先读 PC，别猜硬件。** `0xfffffffe` = 从空 Flash 取向量。
3. **串口认准 USART6 / PC6 PC7 / COM7 / 115200 8N1**，别再看 UART5 的旧文档。
4. **APP 的 `main()` 不能空 `while(1)` 忙等**，否则饿死低优先级线程（RT-Thread 用 `mdelay` 或信号量）。
5. **IAP 前必须 `objcopy` 生成新 `.bin`**，构建不自动出。
6. **升级时让脚本独占 COM**，先关串口助手；ST-Link 报 `LIBUSB_ERROR_ACCESS` 先 `taskkill openocd.exe`。
7. **从运行中的 APP 升级**：APP_test 已实现 `0x13` 重入（自动触发），直接跑 `iap_host_bms.py` 即可；RTT1 还没有。
