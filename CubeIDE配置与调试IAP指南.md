# STM32CubeIDE 下配置并调试 IAP（Bootloader + APP）完整步骤

> 适用：把 CubeMX 工程生成/迁移到 **STM32CubeIDE**，配置成可用的 IAP，并解决"调试 APP 停在 bootloader、无调试信息"的问题。
> 实例值以本工程（STM32F407ZGT6，Bootloader@0x08000000 / APP@0x08010000）为准。

---

## 一、用 CubeIDE 生成工程

1. CubeMX 里分别配置两个工程（外设照旧）：
   - **Bootloader_test**：UART5(PC12=TX/PD2=RX,115200) + CRC + HSE 168MHz
   - **APP_test**：UART5 + GPIO(LED0=PE3/LED1=PE4) + HSE 168MHz
2. 生成时 **Project Manager → Toolchain 选 "STM32CubeIDE"** → Generate Code。
3. 用 STM32CubeIDE 打开工程（workspace 里 Import 或直接打开）。

> 生成后工程根目录会出现：`.cproject`、`.project`、`.settings/`、`STM32F407ZGTX_FLASH.ld`、`STM32F407ZGTX_RAM.ld`。

---

## 二、恢复 IAP 代码（重新生成后必做）

CubeMX 重新生成会**只保留 USER CODE 保护区**内的代码，保护区之外的改动会被冲掉。需要检查/恢复：

### 1. Bootloader：确认 `iap.c` / `iap.h` 还在
它们不是 CubeMX 管理的文件，一般还在，位置不变：
- `Core/Src/iap.c`
- `Core/Inc/iap.h`

CubeIDE 把 `Core` 作为源目录，**`Core/Src` 下的 .c 会自动编译**，`Core/Inc` 会自动作为头文件搜索路径，无需额外配置。

### 2. Bootloader：`main.c` 补 `#include "iap.h"`（关键！）
重新生成会重置 main.c 顶部的 Includes 区，`#include "iap.h"` 会被删掉。
- **现象**：编译报 `implicit declaration of function 'iap_init'` 等警告/错误。
- **修复**：把 include 加进 **USER CODE 保护区**，以后重新生成不再丢：
  ```c
  /* Private includes ----------------------------------------------------------*/
  /* USER CODE BEGIN Includes */
  #include "iap.h"     /* 放保护区：CubeMX 重新生成不会删掉 */
  /* USER CODE END Includes */
  ```

### 3. Bootloader：确认 main.c 的 USER CODE 2（IAP 主流程）保留
```c
/* USER CODE BEGIN 2 */
iap_init();
if (!iap_wait_handshake(2000) && iap_app_valid())
  iap_jump_to_app();   /* 不复返 */
iap_update_loop();     /* 收到 JUMP 后内部跳转，不返回 */
/* USER CODE END 2 */
```

### 4. APP：确认 main.c 的 USER CODE 1（跳转必需三行）保留
```c
/* USER CODE BEGIN 1 */
  SCB->VTOR = FLASH_BASE | 0x10000U;
HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
__enable_irq();   /* bootloader 跳转前关过中断，APP 必须重新使能，否则 SysTick/HAL_GetTick 停摆 */
/* USER CODE END 1 */
```

---

## 三、改链接脚本（CubeIDE 工程）

CubeIDE **没有图形化的起始地址设置**，就是改 `.ld` 文本。

- **Bootloader_test**：`STM32F407ZGTX_FLASH.ld` —— **不用改**（默认 0x08000000，正确）。
- **APP_test**：`STM32F407ZGTX_FLASH.ld` —— **必须改**，把 MEMORY 段 FLASH 改为：
  ```
  MEMORY
  {
    CCMRAM    (xrw)    : ORIGIN = 0x10000000,   LENGTH = 64K
    RAM    (xrw)    : ORIGIN = 0x20000000,   LENGTH = 128K
    /* APP 分区：起始 0x08010000（bootloader 占前 64KB），容量 960KB */
    FLASH    (rx)    : ORIGIN = 0x08010000,   LENGTH = 0xEF000
  }
  ```
- 验证：编译后 `Debug/APP_test.elf` 的 `.isr_vector` VMA 应为 `0x08010000`。

> ⚠️ CubeMX 每次重新生成可能把 `.ld` 重置回 0x08000000，改完尽量别再触发重生成，或每次重生成后复查这行。

---

## 四、编译

1. 右键工程 → **Build Project**（或 Ctrl+B），两工程各编一次。
2. 确认 **0 error**。产物在 `Debug/`（`Bootloader_test.elf`、`APP_test.elf`）。

---

## 五、（可选）生成 .bin 供串口 IAP 脚本用

右键工程 → **Properties → C/C++ Build → Settings**：
- 方法 A：**MCU Post build outputs** → 勾选 "Convert to binary file (.bin)"。
- 方法 B：**Build Steps → Post-build steps** 填：
  ```
  ${cross_cc_objcopy} -O binary "${ProjName}.elf" "${ProjName}.bin"
  ```

---

## 六、首次烧录（两个固件，两个不同地址）

- **Bootloader** → 地址 `0x08000000`：右键 Bootloader_test → Debug As 即可（它链接就在这）。
- **APP** → 地址 `0x08010000`：CubeIDE Debug APP 会自动用你 `.ld` 的地址（0x08010000）下载；或用 STM32CubeProgrammer 指定地址烧 `APP_test.bin`。

烧录顺序建议：先 bootloader，再 APP。烧完板子复位 → 2 秒后应自动跳 APP，LED 交替闪烁。

---

## 七、调试 APP 的正确启动配置（避开 bootloader）

**问题**：直接 Debug APP 工程，芯片复位后从 `0x08000000` 启动进 bootloader（如 `Reset_Handler@0x080012E8`），而 APP 符号表解析不了 → 停在无源码地址。

**配置**（让调试器从 APP 复位入口启动）：

1. 菜单 **Run → Debug Configurations** → 选 APP_test。
2. **Startup** 页，启动方式选：
  - **Specify vector table (hex)** 填 **`0x08010000`**（推荐：调试器从该向量表读取 APP 的 SP 和 Reset_Handler）
  - 或 **Set program counter (hex)** 填编译后 APP ELF 中实际的 Reset_Handler 地址（注意这只设 PC 不设 SP）
3. 保持勾选 **"Set breakpoint at: main"**，地址以当前 APP ELF 符号为准。
4. Apply → Debug。

> Bootloader_test 工程调试**不要**改，保持 **Default start address**（它就该从 0x08000000 启动）。

---

## 八、消除"每次烧录报 Break at 0x080012e8 无调试信息"

**成因**：Debug 下载后 CubeIDE 会复位芯片，复位瞬间 PC 停在 bootloader 的 Reset_Handler（0x080012E8），APP 符号表解析不了 → 提示。随后 Startup 配置把你切到 APP，程序正常。**无害**。

要让它消失（让 bootloader 区也有符号），给调试会话加载 bootloader 的 elf：

### 方式 A（运行时敲一次，最稳）
Debug 起来后，在 CubeIDE 底部 Debug 视图**多个控制台里选标 `gdb` 的那个**，敲：
```
add-symbol-file "D:/dkt_stm32/Boot_test/Bootloader_test/Debug/Bootloader_test.elf"
```
成功后 0x080012E8 显示为 `Reset_Handler`，提示消失。

### 方式 B（写进配置，每次自动加载）
1. `Run → Debug Configurations → APP_test → Startup` 页。
2. 找 **gdb 命令输入框**（不同版本叫 "Execute below lines" / 初始化命令；**不是** Debugger 页的 GDB Server command line——那是给 gdbserver 的参数，不是 gdb 命令）。
3. 填（路径用**正斜杠**，加引号）：
   ```
   add-symbol-file "D:/dkt_stm32/Boot_test/Bootloader_test/Debug/Bootloader_test.elf"
   ```
4. Apply → Debug。
5. 若找不到输入框：把 Startup 页里看到的字段发我确认，或退回方式 A / 直接忽略提示。

> 路径若换目录/工程名变了要同步更新。bootloader 重新编译不用改（路径不变）。

---

## 九、验证整条 IAP 链路（不依赖调试器）

1. 用串口脚本实测升级（会覆盖调试器行为，最真实）：
   ```bash
  python tools/iap_host_bms.py --port COM7 --bin APP_test/Debug/APP_test.bin
   ```
2. 升级完成跳转后，应看到 LED 闪烁方式变化（说明新 APP 生效）。
3. GUI 首次 HELLO 无响应时会发送 APP 升级触发帧，APP 校验通过后软复位回 bootloader。

---

## 附：地址速查（本工程实测值）

| 项 | 地址 |
|----|------|
| Bootloader Reset_Handler | 0x080012E8（thumb: 0x080012E9） |
| APP 链接基址 / 向量表 | 0x08010000 |
| APP Reset_Handler | 以当前 ELF/map 为准 |
| APP main | 以当前 ELF/map 为准 |
| APP 元数据 | 0x080FFF00 |
