#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BMS 风格 IAP 上位机升级工具
协议：CRC16-Modbus + 序列号 + HELLO/BEGIN/DATA/END/RUN 五命令
帧格式：B5 4D | VER(1) | CMD(1) | SEQ_L SEQ_H(LE) | LEN_L LEN_H(LE) | DATA[LEN] | CRC16_L CRC16_H(LE)
"""
import argparse
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("错误：缺少 pyserial，请先执行: pip install pyserial")

MAGIC = b"\xB5\x4D"
VERSION = 1
MAX_PAYLOAD_LEN = 256
APP_START_ADDR = 0x08010000
APP_MAX_SIZE = 0x0EFF00   # 960KB

# 命令字
CMD_HELLO   = 0x30
CMD_BEGIN   = 0x31
CMD_DATA    = 0x32
CMD_END     = 0x33
CMD_RUN     = 0x34
MSG_ACK     = 0x02
MSG_NACK    = 0x03

# 超时参数
FRAME_TIMEOUT   = 1.0
SESSION_IDLE    = 5.0
IMAGE_READY_WAIT = 6.0

# ==================== CRC 计算 ====================
def crc16_modbus(data: bytes) -> int:
    """CRC16-Modbus: poly=0xA001, init=0xFFFF"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

def crc32_zlib(data: bytes) -> int:
    """CRC32-IEEE（与 zlib.crc32 一致，反射多项式）"""
    return zlib.crc32(data) & 0xFFFFFFFF

# ==================== APP 复位触发帧 ====================
# 使用 B5 4D 协议的 ENTER_BOOTLOADER (0x13) 命令
BOOT_CMD_ENTER_BOOT = 0x13

def _build_enter_bootloader_frame() -> bytes:
    """构造进入 Bootloader 命令帧：B5 4D | VER | 0x13 | SEQ | LEN=0 | CRC16"""
    seq = 0x0000  # 初始序列号
    prefix = struct.pack("<BBHH", VERSION, BOOT_CMD_ENTER_BOOT, seq, 0)
    crc = crc16_modbus(prefix)
    return MAGIC + prefix + struct.pack("<H", crc)

_APP_TRIGGER_FRAME = _build_enter_bootloader_frame()
del _build_enter_bootloader_frame, BOOT_CMD_ENTER_BOOT

# ==================== 帧编码/解码 ====================
def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    """编码一帧：MAGIC + PREFIX + PAYLOAD + CRC16"""
    if len(payload) > MAX_PAYLOAD_LEN:
        raise ValueError(f"payload too large: {len(payload)} > {MAX_PAYLOAD_LEN}")
    prefix = struct.pack("<BBHH", VERSION, msg_type, seq & 0xFFFF, len(payload))
    crc = crc16_modbus(prefix + payload)
    return MAGIC + prefix + payload + struct.pack("<H", crc)

def decode_frame(data: bytes) -> tuple:
    """解码一帧，返回 (msg_type, seq, payload) 或 None
    帧格式：B5 4D | VER(1) | MSG_TYPE(1) | SEQ(2 LE) | LEN(2 LE) | DATA[LEN] | CRC16(2 LE)
    共 10 + LEN 字节。"""
    if len(data) < 10:
        return None
    if data[:2] != MAGIC:
        return None
    ver, cmd, seq, plen = struct.unpack_from("<BBHH", data, 2)
    if ver != VERSION or plen > MAX_PAYLOAD_LEN:
        return None
    if len(data) < 10 + plen:
        return None
    payload = data[8:8+plen]
    recv_crc = struct.unpack_from("<H", data, 8+plen)[0]
    calc_crc = crc16_modbus(data[2:8+plen])
    if recv_crc != calc_crc:
        return None
    return (cmd, seq, payload)

# ==================== 帧解析器（处理粘包/拆包） ====================
class FrameParser:
    def __init__(self):
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list:
        """喂入数据，返回解析出的帧列表"""
        self._buffer.extend(data)
        frames = []
        while True:
            idx = self._buffer.find(MAGIC)
            if idx < 0:
                if len(self._buffer) > 1:
                    del self._buffer[:-1]
                return frames
            if idx > 0:
                del self._buffer[:idx]

            if len(self._buffer) < 8:
                return frames

            ver, cmd, seq, plen = struct.unpack_from("<BBHH", self._buffer, 2)
            if ver != VERSION or plen > MAX_PAYLOAD_LEN:
                del self._buffer[0]
                continue

            frame_len = 10 + plen
            if len(self._buffer) < frame_len:
                return frames

            payload = bytes(self._buffer[8:8+plen])
            recv_crc = struct.unpack_from("<H", self._buffer, 8+plen)[0]
            calc_crc = crc16_modbus(bytes(self._buffer[2:8+plen]))
            del self._buffer[:frame_len]

            if recv_crc != calc_crc:
                continue

            frames.append((cmd, seq, payload))
        return frames

# ==================== 串口通信类 ====================
class IapClient:
    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.parser = FrameParser()
        self._seq = 0xFFFF  # _next_seq 第一次调用返回 0（Bootloader 期望从 0 开始）

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFFFF
        return self._seq

    def connect(self):
        self.ser = serial.Serial(self.port, self.baudrate, timeout=0.5)
        print(f"[连接] {self.port} @ {self.baudrate}")

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def send(self, frame: bytes):
        """发送一帧"""
        if not self.ser:
            raise RuntimeError("未连接")
        self.ser.write(frame)
        self.ser.flush()

    def wait_response(self, expect_cmd: int, timeout: float = 2.0) -> tuple:
        """等待指定命令的应答，返回 (msg_type, seq, payload)
        设备应答帧 payload 布局（6字节）:
          [0:2] echo_seq(LE)  [2] cmd  [3] result  [4:6] expected_seq(LE)"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                for msg_type, seq, payload in self.parser.feed(chunk):
                    if len(payload) >= 4 and payload[2] == expect_cmd:
                        return (msg_type, seq, payload)
        return None

    def _reply_ok(self, r, name: str) -> bool:
        """检查应答是否为 ACK 且 result==OK，处理 BAD_SEQ 自动重试"""
        if not r:
            print(f"[{name}] 无应答")
            return False
        msg_type, seq, payload = r
        result = payload[3] if len(payload) >= 4 else 0xFF
        if msg_type != MSG_ACK or result != 0:
            if len(payload) >= 6:
                # 读取 bootloader 期望的序列号
                exp_seq = payload[4] | (payload[5] << 8)
                print(f"[{name}] NACK result=0x{result:02X} expected_seq={exp_seq}")
                self._seq = exp_seq - 1  # 下次 _next_seq() 将返回正确值
            else:
                print(f"[{name}] NACK result=0x{result:02X}")
            return False
        return True

    def hello(self) -> bool:
        """发送 HELLO，返回是否成功"""
        seq = self._next_seq()
        self.send(encode_frame(CMD_HELLO, seq))
        r = self.wait_response(CMD_HELLO, timeout=1.0)
        if self._reply_ok(r, "HELLO"):
            print(f"[HELLO] 成功 (seq={seq})")
            return True
        return False

    def begin(self, image_len: int, image_crc: int) -> bool:
        """发送 BEGIN，触发 bootloader 擦除"""
        seq = self._next_seq()
        payload = struct.pack("<II", image_len, image_crc)
        self.send(encode_frame(CMD_BEGIN, seq, payload))
        r = self.wait_response(CMD_BEGIN, timeout=10.0)
        if self._reply_ok(r, "BEGIN"):
            print("[BEGIN] 成功，开始擦除（约 3-5 秒）")
            return True
        return False

    def data(self, offset: int, chunk: bytes) -> bool:
        """发送 DATA 帧"""
        seq = self._next_seq()
        payload = struct.pack("<I", offset) + chunk
        self.send(encode_frame(CMD_DATA, seq, payload))
        r = self.wait_response(CMD_DATA, timeout=1.0)
        if self._reply_ok(r, "DATA"):
            return True
        print(f"[DATA @{offset:#010x}] 失败")
        return False

    def end(self) -> bool:
        """发送 END，触发校验"""
        seq = self._next_seq()
        self.send(encode_frame(CMD_END, seq))
        r = self.wait_response(CMD_END, timeout=5.0)
        if self._reply_ok(r, "END"):
            print("[END] 校验通过")
            return True
        return False

    def run(self) -> bool:
        """发送 RUN，触发跳转"""
        seq = self._next_seq()
        self.send(encode_frame(CMD_RUN, seq))
        r = self.wait_response(CMD_RUN, timeout=1.0)
        if self._reply_ok(r, "RUN"):
            print("[RUN] 已下发，设备即将运行新固件")
            return True
        return False

# ==================== 升级流程 ====================
def upgrade(port: str, bin_path: str):
    """完整升级流程"""
    # 1. 读固件
    print(f"[加载] {bin_path}")
    with open(bin_path, "rb") as f:
        image = f.read()
    image = image + b"\xff" * ((-len(image)) % 4)
    image_crc = crc32_zlib(image)
    print(f"[固件] 大小={len(image)}B  CRC32=0x{image_crc:08X}")

    # 2. 连接
    client = IapClient(port)
    try:
        client.connect()

        # 3. HELLO（带重试，兼容 APP 复位后需要时间）
        print("\n[阶段 1/5] HELLO 握手...")
        for i in range(10):
            if client.hello():
                break
            if i == 0:
                print("  APP 未响应，发送升级触发帧...")
                client.send(_APP_TRIGGER_FRAME)
                print("  等待 APP 软复位（约 1s）...")
                time.sleep(1.0)
                client.parser._buffer.clear()
            time.sleep(0.3)
        else:
            print("[错误] HELLO 失败，请确认设备已进入 bootloader 模式")
            return False

        # 4. BEGIN
        print(f"\n[阶段 2/5] BEGIN 擦除 APP 区...")
        if not client.begin(len(image), image_crc):
            print("[错误] BEGIN 失败")
            return False

        # 5. DATA（分包发送）
        print(f"\n[阶段 3/5] DATA 写入固件...")
        chunk_size = MAX_PAYLOAD_LEN - 4   # 252 = 256(上限) - 4(offset 前缀)
        total = len(image)
        for off in range(0, total, chunk_size):
            chunk = image[off:off+chunk_size]
            ok = client.data(off, chunk)
            if not ok:
                print(f"[错误] 写入失败 @ 0x{off:08x}")
                return False
            if off % (chunk_size * 20) == 0:
                print(f"  进度: {off}/{total} ({100*off//total}%)")
        print(f"  进度: {total}/{total} (100%) [OK]")

        # 6. END（校验）
        print(f"\n[阶段 4/5] END 校验 CRC...")
        if not client.end():
            print("[错误] END 校验失败")
            return False

        # 7. RUN（跳转）
        print(f"\n[阶段 5/5] RUN 跳转 APP...")
        client.run()

        print("\n[OK] Upgrade successful!")
        return True

    except Exception as e:
        print(f"[错误] {e}")
        return False
    finally:
        client.close()

# ==================== 主入口 ====================
def main():
    parser = argparse.ArgumentParser(
        description="BMS 风格 IAP 上位机升级工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python iap_host_bms.py --port COM3 --bin APP_test/Debug/APP_test.bin
  python iap_host_bms.py --port /dev/ttyUSB0 --bin firmware.bin

协议说明:
  - 帧格式: B5 4D | VER | CMD | SEQ(LE) | LEN(LE) | DATA | CRC16(LE)
  - 命令: HELLO(0x30) BEGIN(0x31) DATA(0x32) END(0x33) RUN(0x34)
  - CRC: CRC16-Modbus (poly=0xA001, init=0xFFFF)
  - 镜像 CRC: CRC32-IEEE (与 zlib.crc32 一致)
        """
    )
    parser.add_argument("--port", required=True, help="串口号，如 COM3 或 /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    parser.add_argument("--bin", required=True, help="APP 固件 .bin 文件路径")
    args = parser.parse_args()

    success = upgrade(args.port, args.bin)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
