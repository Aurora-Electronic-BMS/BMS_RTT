# STM32CubeIDE + ST-Link：以 CMake 为唯一构建源的 APP 编译、烧录与调试

本文只说明 APP（`APP/`）的工作流。STM32CubeIDE 负责调用 CMake、驱动 ST-Link 和
提供 GDB 调试界面；编译选项、链接地址、产物生成规则均以仓库中的 CMake 文件为准，**不要**
再把 CubeIDE Managed Build、CubeMX 生成的 Makefile 或 CubeIDE 的 Post-build step 当作第二套
构建规则。

> 本流程面向 STM32F407VGT6。Bootloader 已占用 `0x08000000..0x0800FFFF`；APP 从
> `0x08010000` 开始。若需要同时更新 Bootloader，请按 `Bootloader/README.md` 中的 Bootloader
> 流程单独构建和烧录，不能把 APP 镜像烧到 `0x08000000`。

## 1. 构建边界与生成产物

APP 的唯一构建入口为：

```text
APP/CMakeLists.txt
APP/CMakePresets.json
APP/cmake/gcc-arm-none-eabi.cmake
```

`Debug` 与 `Release` 预设使用 Ninja，构建目录分别为 `APP/build/Debug` 和
`APP/build/Release`。`CMakeLists.txt` 中的 `firmware_artifacts` 默认目标会在链接完成后自动
调用 `tools/package_app_image.py`，因此无需在 CubeIDE 中再配置 Post-build 命令。

以 Debug 构建为例，最终产物如下：

| 文件 | 生成位置 | 用途 |
| --- | --- | --- |
| `BMS_RTT.elf` | `APP/build/Debug/` | CubeIDE/GDB 调试符号文件 |
| `BMS_RTT.bin` | `APP/build/Debug/` | 上位机 `Firmware Upgrade`，仅包含 APP 本体 |
| `BMS_RTT.metadata.bin` | `APP/build/Debug/` | Bootloader 有效性 metadata，地址为 `0x080FFF00` |
| `BMS_RTT_stlink.hex` | `APP/build/Debug/` | ST-Link 直烧镜像，包含 APP 与 metadata |

镜像和地址的对应关系不可混用：

```text
上位机串口升级  -> BMS_RTT.bin
ST-Link 直接烧录 -> BMS_RTT_stlink.hex
CubeIDE 调试符号 -> BMS_RTT.elf
```

`_stlink.hex` 中同时含有：

```text
APP       : 0x08010000 起
metadata  : 0x080FFF00（20 字节）
```

Bootloader 会读取 metadata 并对 APP 做 CRC32 校验。直接把普通 `.elf` 或裸 `.bin` 下载到
APP 区，会遗漏 metadata；下次复位时 Bootloader 可能判定 APP 无效。

## 2. 前置条件

在启动 CubeIDE 前，确认以下程序在其可见的 `PATH` 中：

```text
cmake >= 3.22
ninja
python3
arm-none-eabi-gcc / arm-none-eabi-objcopy
```

本仓库的工具链文件 `APP/cmake/gcc-arm-none-eabi.cmake` 直接按上述名称寻找 ARM GNU
Toolchain；若 CubeIDE 没有继承系统 `PATH`，在 CubeIDE 的 CMake/环境设置中补充 ARM GNU
Toolchain 的 `bin` 目录即可。还应确认 CubeIDE 能通过 ST-Link 以 SWD 连接目标板。

### Windows：使用 winget 安装构建依赖

先从 STMicroelectronics 安装包安装 STM32CubeIDE。随后打开 PowerShell（如安装器要求管理员权限，
请以管理员身份启动），执行：

```powershell
winget install --exact --id Kitware.CMake --source winget
winget install --exact --id Ninja-build.Ninja --source winget
winget install --exact --id Python.Python.3.13 --source winget
winget install --exact --id Arm.GnuArmEmbeddedToolchain --source winget
```

安装完成后关闭并重新打开 PowerShell 和 STM32CubeIDE，使新的 `PATH` 生效；然后执行：

```powershell
cmake --version
ninja --version
python --version
arm-none-eabi-gcc --version
arm-none-eabi-objcopy --version
```

五条命令都应能输出版本号。Windows 上 Python 命令通常为 `python`，CMake 的
`find_package(Python3)` 会使用该 Python 3 解释器；不要求命令名必须是 `python3`。

若最后两条命令提示找不到，将实际安装的 ARM GNU Toolchain `bin` 目录加入 Windows 的用户
`Path`，然后重新启动 CubeIDE。例如（将路径替换为实际安装目录）：

```powershell
$armGnuBin = 'C:\path\to\arm-gnu-toolchain\bin'
[Environment]::SetEnvironmentVariable(
    'Path',
    [Environment]::GetEnvironmentVariable('Path', 'User') + ";$armGnuBin",
    'User'
)
```

本节的 ARM 工具链用于 CMake 编译；ST-Link 下载还需要 STM32CubeIDE 随附或另外安装的
STM32CubeProgrammer CLI，供第 4 节的 External Tool 调用。

首次在终端验证环境时，可在 CubeIDE 内置 Terminal 或系统终端执行：

```bash
cd /Users/cc/Documents/3.GithubDoc/BMS_Software/APP
cmake --preset Debug
cmake --build --preset Debug --parallel
```

输出中应包含 `Packaging APP binary, Bootloader metadata, and ST-Link HEX`，并在
`APP/build/Debug/` 得到上表四个文件。该命令也是排查 CubeIDE 配置问题时的基线；若命令行不
能通过，先修复 CMake、Ninja、Python 或 ARM 工具链，不要改用 CubeIDE Managed Build 绕过它。

## 3. 将 APP 作为 CMake 项目导入 CubeIDE

较新的 STM32CubeIDE 可直接导入既有 CMake 源码。选择：

```text
File
  → STM32 Project Create/Import（或 New → STM32 CMake Project）
  → STM32 CMake Project / Project with existing CMake sources
```

填写：

| 项目项 | 值 |
| --- | --- |
| Source directory | `/Users/cc/Documents/3.GithubDoc/BMS_Software/APP` |
| MCU | `STM32F407VGT6` |
| CMake toolchain | `APP/cmake/gcc-arm-none-eabi.cmake` |
| Debug build directory | `APP/build/Debug` |
| Debug generator | `Ninja` |
| Debug CMake cache | `CMAKE_BUILD_TYPE=Debug` |

如果所用 CubeIDE 版本提供预设选择，直接选择 `Debug` configure/build preset；否则按上表配置，
使其等价于：

```bash
cmake --preset Debug
cmake --build --preset Debug --parallel
```

完成导入后，使用 **Project → Build Project** 编译。CubeIDE 首次构建会先执行 CMake configure，
之后复用 `APP/build/Debug` 的 CMake cache。不要在 Project Properties 中手动补充源码、宏、
链接脚本或 post-build 命令，这些内容已由 CMake 管理。

若本机 CubeIDE 版本没有 “STM32 CMake Project” 导入项，可将 `APP/` 以普通 C/C++ 外部构建
项目导入，并把 Build command 设为 `cmake --build --preset Debug --parallel`；首次或修改
`CMakeLists.txt` 后，先运行 `cmake --preset Debug`。无论哪种导入方式，CMake 文件仍是唯一
构建源。

### CubeMX 重新生成代码后的处理

`APP/BMS_RTT.ioc` 仅用于外设配置和代码生成，不能替代 CMake 构建入口。重新生成代码后：

1. 检查 `APP/` 的改动；保留项目已有的 APP 起始地址、向量表偏移和链接脚本设置。
2. 若新增或删除源文件、头文件目录或 HAL 模块，同步检查
   `APP/cmake/stm32cubemx/CMakeLists.txt` 与顶层 `APP/CMakeLists.txt`。
3. 重新执行 Debug 构建，并确认 `_stlink.hex` 被重新生成。

## 4. 在 CubeIDE 中通过 ST-Link 烧录 APP

### 4.1 推荐方式：CubeIDE External Tool 调用 STM32CubeProgrammer CLI

在 CubeIDE 中创建一个 External Tool（Program），程序选择 `STM32_Programmer_CLI`：Windows
使用 `STM32_Programmer_CLI.exe`；macOS/Linux 可选择相应的 CLI 或 shell 启动脚本。工作目录设为
APP 工程目录，参数为：

```text
-c port=SWD -w ${project_loc}/build/Debug/BMS_RTT_stlink.hex -v -rst
```

其中 `${project_loc}` 应解析到 `.../BMS_Software/APP`；如果项目名称或变量解析方式不同，改为
该目录的绝对路径。执行前先 Build Project，确保 HEX 来自当前 ELF。

`-w` 会按 HEX 中记录的地址写入，`-v` 校验，`-rst` 复位。**不要添加 `-e all` 或 Full chip
erase**，否则会擦除位于 Sector 0~3 的 Bootloader。

只有在需要彻底清空旧 APP 区时，才在写入前显式擦除 Sector 4~11：

```text
-c port=SWD -e [4 5 6 7 8 9 10 11] -w ${project_loc}/build/Debug/BMS_RTT_stlink.hex -v -rst
```

这条命令不会触碰 Bootloader 所在的 Sector 0~3。

### 4.2 图形界面方式

也可从 CubeIDE 启动 STM32CubeProgrammer 图形界面，连接方式选 ST-LINK/SWD，选择
`BMS_RTT_stlink.hex` 后执行下载与校验。仍须避免 Full chip erase；如需预擦除，仅选择 Sector
4~11。

烧录并复位后的预期路径为：

```text
MCU 复位
  → Bootloader（0x08000000）
  → 读取 0x080FFF00 的 metadata
  → 校验 0x08010000 起 APP 的 CRC32
  → 校验通过后跳转 APP
```

## 5. 在 CubeIDE 中调试 APP

调试使用 ELF 的符号信息，但不能让默认的 Debug 下载动作改为写入普通 ELF。推荐采用
“**先烧录 HEX，再 attach ELF**”的方式。

1. 按第 3 节编译，确认 `APP/build/Debug/BMS_RTT.elf` 和
   `BMS_RTT_stlink.hex` 已更新。
2. 按第 4 节用 ST-Link 写入 `_stlink.hex`。
3. 创建 `STM32 C/C++ Application` Debug Configuration：

   | 配置项 | 值 |
   | --- | --- |
   | C/C++ Application | `APP/build/Debug/BMS_RTT.elf` |
   | Debug probe | ST-LINK，接口 SWD |
   | 调试方式 | Attach to running target；或关闭 “Load image/download executable” |
   | 断点 | 可在 `main` 或需要观察的源代码行设置 |

4. 启动调试后，CubeIDE 以 `BMS_RTT.elf` 解析断点、变量、调用栈和寄存器；目标 Flash 保持第
   4 节写入的完整 HEX，不会遗漏 metadata。

如果 Debug Configuration 必须在复位后启动，复位会先停在 Bootloader。可在其 GDB 初始化/启动
命令区加入以下命令，手动转入 APP 向量表；随后再运行到 `main`：

```gdb
monitor reset halt
set {unsigned int}0xE000ED08 = 0x08010000
set $sp = *(unsigned int *)0x08010000
set $pc = *(unsigned int *)0x08010004
```

这里 `0xE000ED08` 是 VTOR，后两行从 APP 向量表载入初始栈指针和复位入口。正常 Attach 到已由
Bootloader 启动的 APP 时不需要这些命令。

## 6. 快速检查清单

- [ ] CubeIDE 的 Debug 构建实际执行的是 `APP/CMakeLists.txt`，而不是 Managed Build。
- [ ] 产物位于 `APP/build/Debug/`，且本次构建已生成 `.elf`、`.bin`、`.metadata.bin` 和
  `_stlink.hex`。
- [ ] ST-Link 烧录的是 `BMS_RTT_stlink.hex`，没有使用 `.elf` 或上位机专用 `.bin`。
- [ ] 未执行 Full chip erase；如需擦除，只擦 Sector 4~11。
- [ ] 调试器加载的符号文件是 `BMS_RTT.elf`，并禁用了默认 ELF 下载或使用 Attach。
