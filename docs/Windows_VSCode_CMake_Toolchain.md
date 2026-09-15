# Windows + VS Code：BMS_Software CMake 工具链、编译、烧录与调试指南

本文面向 Windows 10/11 64 位系统，使用 VS Code、CMake 和 Ninja 构建本仓库的 STM32F407VGT6
APP 与 Bootloader。构建配置完全来自仓库中的 CMake 文件和预设；不要切换到 CubeIDE Managed Build
或另行维护 Makefile。

> **先读这一条。** APP 的 Bootloader 位于 `0x08000000..0x0800FFFF`，APP 从
> `0x08010000` 开始。使用 ST-Link 直烧 APP 时，必须使用
> `APP/build/Debug/BMS_RTT_stlink.hex`，不能使用普通 `.elf` 或上位机升级专用的 `.bin`。

## 1. 构建对象、目录和产物

| 对象 | CMake 源目录 | 配置预设 | Debug 构建目录 |
| --- | --- | --- | --- |
| APP | `APP/` | `Debug` / `Release` | `APP/build/Debug/` |
| Bootloader | `Bootloader/` | `Debug` / `Release` | `Bootloader/build/Debug/` |

两个 `CMakePresets.json` 都要求：CMake >= 3.22、Ninja、Python 3 和 ARM GNU Toolchain。
工具链文件 `APP/cmake/gcc-arm-none-eabi.cmake` / `Bootloader/cmake/gcc-arm-none-eabi.cmake`
按名称调用 `arm-none-eabi-gcc`、`arm-none-eabi-gdb`、`arm-none-eabi-objcopy`，因此 ARM 工具链的
`bin` 目录必须在 `Path` 中。

APP 的 Debug 构建成功后应生成：

| 文件 | 用途 |
| --- | --- |
| `BMS_RTT.elf` | 调试符号文件，供 Cortex-Debug / GDB 使用 |
| `BMS_RTT.bin` | 上位机 `Firmware Upgrade` 的串口升级文件，只含 APP 本体 |
| `BMS_RTT.metadata.bin` | Bootloader 的 APP 有效性 metadata |
| `BMS_RTT_stlink.hex` | ST-Link 直烧文件，包含 APP 和 metadata |

`BMS_RTT_stlink.hex` 中的 APP 位于 `0x08010000`，metadata 位于 `0x080FFF00`。Bootloader
会校验 metadata 与 APP CRC32；把裸 `.bin` 或 `.elf` 写入 APP 区会遗漏 metadata，复位后
Bootloader 可能拒绝跳转到 APP。

## 2. 前置条件

准备以下硬件和软件：

- Windows 10/11 x64；建议以普通用户启动 VS Code，只有安装驱动或软件时才提升权限。
- 已连接的 ST-Link（SWDIO、SWCLK、GND；目标板已供电）。首次连接时等待 Windows 安装 ST-Link
  驱动；若设备管理器无法识别，再通过 STM32CubeProgrammer 修复/更新 ST-Link 驱动。
- 仓库的完整工作副本。下文以 `D:\workspace\BMS_Software` 为例；请替换成实际路径。
- 网络访问，以安装 Windows 软件包和 VS Code 扩展。

除 VS Code、CMake、Ninja、Python、ARM GNU Toolchain 外，烧录与调试还需要：

- **STM32CubeProgrammer**：提供 ST-Link 驱动、`STM32_Programmer_CLI.exe`，可用于可靠地写入
  HEX；从 [ST 官方下载页](https://www.st.com/content/st_com/en/stm32cubeprogrammer.html) 安装 Windows
  版。
- **OpenOCD**：与 Cortex-Debug 配合进行 GDB 调试。本指南推荐 xPack OpenOCD 的 Windows
  压缩包（[官方安装页](https://xpack.github.io/dev-tools/openocd/install/)）；也可以使用团队已验证的
  OpenOCD 发行版。

## 3. 安装基础构建工具

以下命令在 **PowerShell** 中执行。先更新 Windows Package Manager 源：

```powershell
winget source update
```

再安装 VS Code、Git、CMake、Ninja、Python 和 ARM GNU Toolchain：

```powershell
winget install --exact --id Microsoft.VisualStudioCode --source winget
winget install --exact --id Git.Git --source winget
winget install --exact --id Kitware.CMake --source winget
winget install --exact --id Ninja-build.Ninja --source winget
winget install --exact --id Python.Python.3.13 --source winget
winget install --exact --id Arm.GnuArmEmbeddedToolchain --source winget
```

`Python.Python.3.13` 是已验证的 Python 3 包标识；如组织的软件源只提供其他 Python 3 小版本，先
执行下面的命令并安装可用的 `Python.Python.3.*` 项即可：

```powershell
winget search --id Python.Python
```

安装完成后，**关闭所有 PowerShell 和 VS Code 窗口，再打开一个新的 PowerShell**。依次执行：

```powershell
cmake --version
ninja --version
python --version
git --version
arm-none-eabi-gcc --version
arm-none-eabi-gdb --version
arm-none-eabi-objcopy --version
```

每条命令均必须输出版本号。若 `arm-none-eabi-*` 报“不是内部或外部命令”，找到 ARM GNU Toolchain
安装目录下包含 `arm-none-eabi-gcc.exe` 的 `bin` 目录，并将它添加到**用户** `Path`。例如：

```powershell
$armBin = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 Rel1\bin'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
[Environment]::SetEnvironmentVariable('Path', "$userPath;$armBin", 'User')
```

上例版本目录仅为示例，必须以实际安装目录为准。设置后重新打开 PowerShell，再执行
`arm-none-eabi-gcc --version` 确认。不要只在当前终端使用 `$env:Path += ...`；那种做法在重启 VS
Code 后会失效。

## 4. 安装 STM32CubeProgrammer 与 OpenOCD

### 4.1 STM32CubeProgrammer

从 [ST 官方页面](https://www.st.com/content/st_com/en/stm32cubeprogrammer.html) 下载 Windows x64
安装包，运行 `SetupSTM32CubeProgrammer-<版本>.exe` 并完成安装。默认情况下，CLI 位于：

```text
C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe
```

用下列命令验证路径；安装到其他位置时相应替换：

```powershell
$cubeProg = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
Test-Path $cubeProg
& $cubeProg --version
```

第一个命令应返回 `True`。这个工具可读写 Intel HEX、ELF 和 BIN，并支持 ST-Link/SWD；本工程中
推荐它用于烧录，避免手工输入地址。

### 4.2 OpenOCD

从 [xPack OpenOCD Windows 安装说明](https://xpack.github.io/dev-tools/openocd/install/) 下载 Windows
x64 archive，解压到不含空格和中文的目录，例如 `C:\tools\xpack-openocd`。确认该目录中能找到
`openocd.exe`，然后把其所在 `bin` 目录加入用户 `Path`：

```powershell
$openOcdBin = 'C:\tools\xpack-openocd\bin'
Test-Path "$openOcdBin\openocd.exe"
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
[Environment]::SetEnvironmentVariable('Path', "$userPath;$openOcdBin", 'User')
```

重新启动 PowerShell 后验证：

```powershell
openocd --version
```

如果解压后的目录含版本号，例如 `C:\tools\xpack-openocd-0.12.0-...`，应把 `$openOcdBin` 改为其中
实际含有 `openocd.exe` 的 `bin` 路径。OpenOCD 仅用于 VS Code 的 GDB 调试和已有 `Flash Firmware`
任务；不需要它也可通过 STM32CubeProgrammer CLI 完成烧录。

## 5. 安装 VS Code 扩展

在新的 PowerShell 中运行：

```powershell
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools
code --install-extension marus25.cortex-debug
```

扩展职责如下：

| 扩展 | 用途 |
| --- | --- |
| `ms-vscode.cpptools` | C/C++ 语义分析、跳转、问题匹配 |
| `ms-vscode.cmake-tools` | 选择/调用 CMake preset、配置和构建 |
| `marus25.cortex-debug` | 通过 GDB + OpenOCD 调试 Cortex-M |

若 `code` 命令未找到，请重新打开 PowerShell。VS Code Windows 安装程序通常会把它加入 `Path`；也可
通过 VS Code 的扩展面板（`Ctrl+Shift+X`）按上述扩展标识逐一安装。

## 6. 首次命令行构建（必须先通过）

先在系统终端完成一次构建。这一步与 VS Code 无关，能区分“工具链问题”和“编辑器配置问题”。

```powershell
Set-Location D:\workspace\BMS_Software\APP
cmake --preset Debug
cmake --build --preset Debug --parallel
```

预期在末尾出现：

```text
Packaging APP binary, Bootloader metadata, and ST-Link HEX
```

然后确认产物：

```powershell
Get-ChildItem .\build\Debug\BMS_RTT.elf, `
              .\build\Debug\BMS_RTT.bin, `
              .\build\Debug\BMS_RTT.metadata.bin, `
              .\build\Debug\BMS_RTT_stlink.hex
```

Bootloader 使用独立的构建目录，命令也必须在其目录中执行：

```powershell
Set-Location D:\workspace\BMS_Software\Bootloader
cmake --preset Debug
cmake --build --preset Debug --parallel
Get-ChildItem .\build\Debug\Bootloader.elf
```

Release 构建只需将两条命令中的 `Debug` 替换为 `Release`。不要在 APP 目录中执行
`cmake --build --preset Debug` 后又期望得到 Bootloader，反之亦然：两者是两个独立 CMake 项目。

### 失败时的定点排查

| 现象 | 原因与处理 |
| --- | --- |
| `cmake` / `ninja` 找不到 | 重新打开终端；确认相应安装目录已在用户 `Path`。 |
| `arm-none-eabi-gcc` 找不到 | ARM GNU Toolchain `bin` 未加入 `Path`，按第 3 节处理。 |
| `Could NOT find Python3` | 执行 `python --version`；必要时重装 Python 3 并勾选/启用 PATH。 |
| `No such file ... package_app_image.py` | 确认仓库完整且根目录存在 `tools\package_app_image.py`。 |
| CMake 缓存仍使用旧编译器 | 删除当前对象的 `build\Debug` 目录后重新 `cmake --preset Debug`；不要删除源码目录。 |

## 7. 在 VS Code 中使用 CMake Tools

从 PowerShell 打开**仓库根目录**：

```powershell
Set-Location D:\workspace\BMS_Software
code .
```

仓库根目录下没有顶层 `CMakeLists.txt`，而 APP 与 Bootloader 是两个独立工程。因此有两种正确用法：

1. **推荐：使用仓库内任务**。它能同时管理 APP 和 Bootloader（见第 8 节）。
2. **使用 CMake Tools 图形界面**：打开 `APP` 或 `Bootloader` 子目录作为单独工作区，再选择对应的
   `Debug` / `Release` preset。

对于单独打开的 `APP` 工作区，按 `Ctrl+Shift+P`，依次执行：

```text
CMake: Select Configure Preset  → Debug
CMake: Configure
CMake: Build
```

状态栏应显示所选 preset。CMake Tools 会读取 `APP/CMakePresets.json`，无需手动指定编译器、生成器、
链接脚本或构建目录。Bootloader 完全相同，只需把打开的文件夹换为 `Bootloader`。

不要在仓库根目录将 CMake Tools 的 source directory 指向根目录；它不存在根 `CMakeLists.txt`，会导致
“does not appear to contain CMakeLists.txt”错误。

## 8. 让现有 VS Code 任务适配 Windows

仓库的 `.vscode/tasks.json` 和 `.vscode/launch.json` 当前包含 macOS 的绝对路径：
`/opt/homebrew/bin/cmake`、`/opt/homebrew/bin/openocd` 以及 ARM GDB 的 `/Applications/...` 路径。
它们在 Windows 上不能直接运行。保留任务标签和参数不变，只需要按下面方式替换可执行文件路径。

### 8.1 `tasks.json`

在 `.vscode/tasks.json` 中全局替换：

| 当前值 | Windows 推荐值 |
| --- | --- |
| `/opt/homebrew/bin/cmake` | `cmake` |
| `/opt/homebrew/bin/openocd` | `openocd` |
| `/opt/anaconda3/bin/conda` | `conda`（仅上位机使用 Conda 时） |

但仅替换 `command` 还不够直观：必须让**运行任务的 VS Code 进程**能找到 ARM GNU Toolchain。
本工程并不需要在任务中手工传入 `-DCMAKE_TOOLCHAIN_FILE`：`cmake --preset Debug` 会读取
`APP/CMakePresets.json`（或 `Bootloader/CMakePresets.json`），其中已经指定
`cmake/gcc-arm-none-eabi.cmake`。该工具链文件再调用 `arm-none-eabi-gcc`、`objcopy` 等目标工具。

因此，Windows 任务必须同时做到两件事：

1. `command` 调用 Windows 上的 `cmake.exe`；
2. 在任务 `options.env.PATH` 中加入 ARM GNU Toolchain 的 `bin` 目录，使 preset 的工具链文件能
   找到 `arm-none-eabi-*`。

### 8.1.1 可直接替换的 Windows CMake 任务

以下内容替换 `.vscode/tasks.json` 中现有的 6 个 CMake 任务（`Configure/Build/Clean Firmware` 和
`Configure/Build/Clean Bootloader`）。其中的 ARM 版本目录必须替换为实际安装目录；如果 CMake 不安装在
`C:\\Program Files\\CMake\\bin`，也应相应修改 `command`。保留现有的 Flash、Host Computer 和
`Build All` 任务不变。

先在 **PowerShell** 中查找当前可用的 `cmake.exe`。第一条命令只输出实际可执行文件的完整路径，
适合直接填入任务；第二条会列出 `Path` 中所有同名程序，便于发现旧版或重复安装：

```powershell
(Get-Command cmake -ErrorAction Stop).Source
where.exe cmake
```

例如第一条输出为：

```text
C:\Program Files\CMake\bin\cmake.exe
```

则在 `tasks.json` 的 `command` 中写成下面的 JSON 转义形式（每个反斜杠写两个）：

```json
"command": "C:\\Program Files\\CMake\\bin\\cmake.exe"
```

如果 `Get-Command` 和 `where.exe` 都找不到 CMake，说明 CMake 尚未加入 Windows `Path`；先完成第 3 节
安装并重新打开 VS Code，再配置任务。此时不要凭记忆填写路径，也不要把 `cmake --version` 的版本输出
当作路径。

```json
[
    {
        "label": "Configure Firmware",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--preset", "Debug"],
        "options": {
            "cwd": "${workspaceFolder}/APP",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "problemMatcher": []
    },
    {
        "label": "Build Firmware",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--build", "--preset", "Debug", "--parallel"],
        "options": {
            "cwd": "${workspaceFolder}/APP",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "dependsOn": "Configure Firmware",
        "dependsOrder": "sequence",
        "group": {"kind": "build", "isDefault": true},
        "problemMatcher": "$gcc"
    },
    {
        "label": "Clean Firmware",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--build", "--preset", "Debug", "--target", "clean"],
        "options": {
            "cwd": "${workspaceFolder}/APP",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "dependsOn": "Configure Firmware",
        "dependsOrder": "sequence",
        "problemMatcher": []
    },
    {
        "label": "Configure Bootloader",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--preset", "Debug"],
        "options": {
            "cwd": "${workspaceFolder}/Bootloader",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "problemMatcher": []
    },
    {
        "label": "Build Bootloader",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--build", "--preset", "Debug", "--parallel"],
        "options": {
            "cwd": "${workspaceFolder}/Bootloader",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "dependsOn": "Configure Bootloader",
        "dependsOrder": "sequence",
        "problemMatcher": "$gcc"
    },
    {
        "label": "Clean Bootloader",
        "type": "process",
        "command": "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "args": ["--build", "--preset", "Debug", "--target", "clean"],
        "options": {
            "cwd": "${workspaceFolder}/Bootloader",
            "env": {
                "PATH": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin;${env:PATH}"
            }
        },
        "dependsOn": "Configure Bootloader",
        "dependsOrder": "sequence",
        "problemMatcher": []
    }
]
```

上述片段是 `"tasks"` 数组中的内容，不是整个文件。将原 6 个同名任务替换为这些对象，或按相同字段
修改原任务；不要把外层方括号再嵌套进原有的 `"tasks": []`。任务链路如下：

```text
VS Code Task
  -> cmake.exe --preset Debug
  -> CMakePresets.json 中的 toolchainFile
  -> cmake/gcc-arm-none-eabi.cmake
  -> arm-none-eabi-gcc / objcopy（由任务 PATH 提供）
```

若第 3 节已把 ARM 工具链永久加入 Windows 用户 `Path`，`options.env` 可省略，`command` 也可以
简写为 `cmake`。不过上述显式配置能避免“从旧终端或旧 VS Code 窗口启动，未继承新 Path”造成的构建失败，
更适合初次配置或团队成员安装位置不同的情况。

任务执行顺序为：

```text
Configure Firmware      -> APP: cmake --preset Debug
Build Firmware          -> APP: cmake --build --preset Debug -j
Flash Firmware          -> 先 Build Firmware，再用 OpenOCD 写入 _stlink.hex

Configure Bootloader    -> Bootloader: cmake --preset Debug
Build Bootloader        -> Bootloader: cmake --build --preset Debug -j
Flash Bootloader        -> 先 Build Bootloader，再写入 Bootloader.elf
```

替换完成后，按 `Ctrl+Shift+P` → `Tasks: Run Task`，先运行 `Build Firmware`，确认成功后再运行
`Flash Firmware`。不要先运行 Flash；它虽然会依赖 Build，但工具链错误会被烧录输出掩盖。

### 8.2 `launch.json`

在 `.vscode/launch.json` 中为两个配置分别替换：

```json
"gdbPath": "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\<实际版本>\\bin\\arm-none-eabi-gdb.exe",
"serverpath": "C:\\tools\\xpack-openocd\\bin\\openocd.exe"
```

JSON 中反斜杠必须写成 `\\`。若已将工具加入 `Path`，也可以写成更便于团队复用的：

```json
"gdbPath": "arm-none-eabi-gdb",
"serverpath": "openocd"
```

APP 的 `Debug APP (OpenOCD)` 是 attach 调试：它使用 `BMS_RTT.elf` 解析符号，并通过启动命令
设置 VTOR、SP 和 PC 到 APP 向量表。首次调试前，应先用第 9 节的流程写入完整
`BMS_RTT_stlink.hex`，不要让调试器以普通 ELF 替换 Flash 内容。

## 9. 烧录流程

### 9.1 APP：推荐 STM32CubeProgrammer CLI

确认 APP 已生成 `BMS_RTT_stlink.hex`，然后在仓库根目录执行：

```powershell
$cubeProg = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
& $cubeProg -c port=SWD -w .\APP\build\Debug\BMS_RTT_stlink.hex -v -rst
```

`-w` 会按 HEX 自带的地址写入，`-v` 校验，`-rst` 复位。**不要加 `-e all`，也不要在
STM32CubeProgrammer 图形界面执行 Full chip erase**，否则会擦除 Bootloader。

只有确实需要预擦除旧 APP 时，才擦除 STM32F407 的 Sector 4 到 11 后再写入：

```powershell
& $cubeProg -c port=SWD -e [4 5 6 7 8 9 10 11] -w .\APP\build\Debug\BMS_RTT_stlink.hex -v -rst
```

### 9.2 APP：已有 OpenOCD 任务

第 8 节已改好 `tasks.json` 后，运行 `Flash Firmware`。该任务会擦除 Sector 4 到 11，随后写入并校验
`APP/build/Debug/BMS_RTT_stlink.hex`；不会擦除 Bootloader 所在的 Sector 0 到 3。

### 9.3 Bootloader

构建成功后再运行 `Flash Bootloader`，或显式执行：

```powershell
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg `
  -c "program Bootloader/build/Debug/Bootloader.elf verify reset exit"
```

Bootloader 写入 `0x08000000` 区域，会覆盖现有 Bootloader。除非确实在更新 Bootloader，不要执行这个
操作；它和更新 APP 是两项不同的烧录任务。

## 10. VS Code 调试 APP

1. 运行 `Build Firmware`。
2. 运行 `Flash Firmware`，或使用第 9.1 节将完整 `_stlink.hex` 写入目标板。
3. 断开并重新连接一次调试会话，确认设备能够经 Bootloader 正常启动 APP。
4. 在 VS Code 的 Run and Debug 面板选择 `Debug APP (OpenOCD)`，按 `F5`。
5. 在 APP 源码中设置断点；暂停后可在 Variables、Watch、Call Stack 和 Registers 中查看状态。

调试时 `BMS_RTT.elf` 只提供符号信息；实际 Flash 必须保持由 `_stlink.hex` 写入的 APP + metadata。
若按 F5 后停在 Bootloader 或无法到达 `main`，先检查 `launch.json` 的 `postLaunchCommands` 与
`overrideRestartCommands` 是否仍保留 APP 的 VTOR/SP/PC 设置。

## 11. 最终验收清单

- [ ] `cmake`、`ninja`、`python`、`arm-none-eabi-gcc`、`arm-none-eabi-gdb`、`arm-none-eabi-objcopy`
  和 `openocd` 均可在新 PowerShell 中运行。
- [ ] `APP` 的 `cmake --preset Debug` 与 `cmake --build --preset Debug --parallel` 成功。
- [ ] `APP/build/Debug/` 中包含 `.elf`、`.bin`、`.metadata.bin`、`_stlink.hex` 四种 APP 产物。
- [ ] `Bootloader` 的 Debug 构建成功（需要构建 Bootloader 时）。
- [ ] VS Code 的 macOS 绝对路径已替换成 Windows 可执行文件名或实际 `.exe` 路径。
- [ ] ST-Link 直烧 APP 使用的是 `BMS_RTT_stlink.hex`；上位机串口升级使用的是
  `BMS_RTT.bin`。
- [ ] 未执行 Full chip erase，且 APP 烧录只擦除了 Sector 4 到 11。

## 12. 官方参考

- [VS Code Windows 安装](https://code.visualstudio.com/docs/setup/windows)
- [VS Code 命令行与扩展安装](https://code.visualstudio.com/docs/configure/command-line)
- [CMake Tools 扩展](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
- [Cortex-Debug 扩展](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug)
- [STM32CubeProgrammer](https://www.st.com/content/st_com/en/stm32cubeprogrammer.html)
