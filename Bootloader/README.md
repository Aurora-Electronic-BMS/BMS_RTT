# BMS Bootloader 升级协议

Bootloader 同时监听 USART1 和 USART3，两路串口配置均为 `115200 8N1`。
收到第一帧合法 `HELLO` 握手命令后，Bootloader 会锁定该串口，本次升级的
ACK、NACK 和固件数据都使用这一路串口。如果检测到有效 APP，Bootloader 将
等待 500 ms 接收握手命令；等待超时后跳转至 APP。
如果 APP 无效，Bootloader 将一直停留在升级模式，等待上位机发送升级命令。
为避免上电初始化或串口切换瞬间丢失单帧数据，上位机应周期性重发 `HELLO`，
直到收到 ACK 后再发送 BEGIN。

APP 正常运行期间也会同时监听 USART1 和 USART3。上位机发送 `0x13` 进入
Bootloader 命令后，APP 会在 RTC 备份寄存器写入升级请求标志并执行软件复位。
Bootloader 检测并清除该标志后，将忽略有效 APP，不再执行 500 ms 超时跳转，
而是一直等待任意一路串口发送合法 `HELLO`。

## 数据帧格式

所有多字节字段均采用小端字节序：

```text
B5 4D | version:u8 | type:u8 | sequence:u16 | payload_length:u16 |
payload | crc16_modbus:u16
```

`version` 固定为 1。CRC16/Modbus 的计算范围从 `version` 开始，到
`payload` 末尾结束，不包含前面的两个魔数。单帧最大负载长度为 260 字节。

升级命令必须使用连续递增的 16 位序号：

| 类型 | 命令 | 负载内容 |
| --- | --- | --- |
| `0x30` | HELLO | 空 |
| `0x31` | BEGIN | `image_length:u32, image_crc32:u32` |
| `0x32` | DATA | `offset:u32, data:1..256 字节` |
| `0x33` | END | 空 |
| `0x34` | RUN | 空 |

`image_crc32` 使用标准 IEEE/zlib CRC-32。反射多项式为 `0xEDB88320`，
初始值和最终异或值均为 `0xFFFFFFFF`。CRC 的计算范围是 APP 原始二进制文件
开头的 `image_length` 个字节。

响应帧使用 `0x02` 表示 ACK，使用 `0x03` 表示 NACK。响应负载格式如下：

```text
command_sequence:u16 | command:u8 | result:u8 | expected_sequence:u16
```

结果码含义如下：

| 结果码 | 含义 |
| --- | --- |
| `0` | 操作成功 |
| `1` | 数据帧 CRC 错误 |
| `2` | 长度或写入偏移错误 |
| `3` | 未知命令 |
| `4` | 升级状态错误 |
| `5` | 帧序号错误 |
| `6` | Flash 擦除、写入或校验错误 |
| `7` | APP 整包 CRC 错误 |
| `8` | APP 栈地址或复位向量错误 |

上位机可以安全地重发最近一次已经成功确认的数据帧，Bootloader 会重新返回 ACK，
不会重复执行该帧对应的 Flash 操作。

握手或传输阶段连续 5 秒没有收到完整帧时，本次会话结束：若 APP 尚未擦除且仍然
有效，则启动原 APP；若 APP 已被擦除，则返回双串口 HELLO 检测，允许重新升级。
END 校验成功后会保留 6 秒确认窗口，重复 END 会再次返回 ACK。收到 RUN 后立即启动
新 APP；RUN 丢失时也会在确认窗口超时后自动启动。

## Flash 地址分配

- Bootloader：`0x08000000..0x0800FFFF`，对应 Sector 0~3
- APP 二进制：`0x08010000..0x080FFEFF`
- APP 有效性元数据：`0x080FFF00..0x080FFFFF`
- 升级时只擦除 Sector 4~11

APP 二进制的最大长度为 `0xEFF00` 字节。所有固件数据及有效性元数据的写入地址
均限制在 `0x08010000..0x080FFFFF` 范围内。

## 掉电保护机制

收到合法的 BEGIN 命令后，Bootloader 擦除 Sector 4~11，同时清除上一版本 APP
的有效性元数据。只有在全部 DATA 接收完成，并且 END 阶段通过整包 CRC、APP
栈地址和复位向量检查后，Bootloader 才会最后写入有效性标志。

因此，如果升级过程中发生断电，APP 有效性检查将无法通过。设备再次上电后会一直
停留在 Bootloader 中，不会跳转到不完整的 APP。

## APP 跳转过程

APP 校验通过后，Bootloader 将执行以下操作：

1. 关闭全局中断。
2. 反初始化 USART、CRC 以及其他 HAL 外设。
3. 关闭 SysTick，并清除所有 NVIC 使能位和挂起标志。
4. 将 `VTOR` 设置为 `0x08010000`。
5. 设置 APP 的 MSP。
6. 恢复全局中断并跳转至 APP 复位向量。

## APP 运行期间请求升级

APP 协议增加以下命令：

| 类型 | 命令 | 负载内容 |
| --- | --- | --- |
| `0x13` | ENTER_BOOTLOADER | 空 |

APP 收到合法命令后先返回 ACK，然后将升级请求魔数及其反码写入 RTC 备份寄存器，
最后调用 `NVIC_SystemReset()`。使用软件复位而不是直接从 APP 跳转 Bootloader，
可以保证时钟、外设、中断和栈回到确定状态。

APP 会锁定首先发送合法协议帧的串口。活动链路持续收到心跳时，另一串口的数据不会
接管通信；心跳超过 1000 ms 后，当前串口被释放，任意一路串口都可以通过下一帧
合法协议数据重新建立连接。

## 上位机自动升级流程

上位机连接串口后，点击 `Firmware Upgrade`，选择 APP 的
`.bin` 文件并确认。上位机首先检查文件长度、栈地址和复位向量，然后自动执行：

1. 向 APP 发送 `ENTER_BOOTLOADER` 并等待设备软件复位。
2. 周期性发送 `HELLO`，直到收到 Bootloader ACK。
3. 发送包含文件长度和 IEEE CRC32 的 `BEGIN`，等待 Sector 4~11 擦除完成。
4. 按每包最多 256 字节发送 `DATA`，每包必须收到 ACK 后才继续。
5. 发送 `END`，等待 Bootloader 完成整包 CRC 和 APP 向量检查；ACK 丢失时重发 END。
6. 收到 END ACK 后发送 `RUN`，Bootloader 启动新 APP。

DATA 或其他命令发生 ACK 丢失时，上位机会使用相同序号重发；Bootloader 对最近一包
成功数据的重复发送会直接再次返回 ACK，不会重复执行 Flash 操作。

使用 APP 的 CMake 工程完成构建后，会在构建目录自动生成 `BMS_V1.0.bin`，该文件
可以直接通过 `Firmware Upgrade` 按钮选择。

如果使用 VS Code 的 `Download APP` / `Flash Firmware` 任务通过 ST-Link 直接烧录
APP，该任务会先擦除 APP 区 Sector 4~11，再写入构建生成的
`BMS_V1.0_stlink.hex`。该 HEX 同时包含 `BMS_V1.0.bin` 的 APP 内容和
`0x080FFF00` 处的 metadata，因此复位后 Bootloader 也能通过同一套 metadata +
CRC32 校验并正常跳转到 APP。
