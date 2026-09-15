#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IAP 固件烧录工具
用法:
    python flash.py --port COM7 --bootloader Bootloader_test.bin --app APP_test.bin
    python flash.py --port COM7 --app APP_test.bin   # 仅烧录 APP（需先有 bootloader）
    python flash.py --port COM7 --bootloader Bootloader_test.bin  # 仅烧录 bootloader
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

# ==================== IAP 协议常量 ====================
MAGIC = b"\xB5\x4D"
VERSION = 1
MAX_PAYLOAD_LEN = 256
APP_START_ADDR = 0x08010000
APP_MAX_SIZE = 0x0EFF00  # 960KB

CMD_HELLO = 0x30
CMD_BEGIN = 0x31
CMD_DATA = 0x32
CMD_END = 0x33
CMD_RUN = 0x34
MSG_ACK = 0x02
MSG_NACK = 0x03

# APP 触发升级帧
_APP_TRIGGER_HEADER = 0xAA55
_APP_TRIGGER_CMD = 0x0001
_APP_TRIGGER_CRC_POLY = 0x1021

def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1) ^ _APP_TRIGGER_CRC_POLY if (crc & 0x8000) else (crc << 1)
    return crc & 0xFFFF

_APP_TRIGGER_FRAME = (
    struct.pack("<HHI", _APP_TRIGGER_HEADER, _APP_TRIGGER_CMD, 0) +
    struct.pack("<H", _crc16_ccitt(struct.pack("<HHI", _APP_TRIGGER_HEADER, _APP_TRIGGER_CMD, 0)))
)

# ==================== CRC 计算 ====================
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

def crc32_zlib(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF

# ==================== 帧编码/解码 ====================
def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD_LEN:
        raise ValueError(f"payload too large: {len(payload)} > {MAX_PAYLOAD_LEN}")
    prefix = struct.pack("<BBHH", VERSION, msg_type, seq & 0xFFFF, len(payload))
    crc = crc16_modbus(prefix + payload)
    return MAGIC + prefix + payload + struct.pack("<H", crc)

class FrameParser:
    def __init__(self):
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list:
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

# ==================== IAP 客户端 ====================
class IapClient:
    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.parser = FrameParser()
        self._seq = 0xFFFF  # 第一次 _next_seq() 返回 0

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
        if not self.ser:
            raise RuntimeError("未连接")
        self.ser.write(frame)
        self.ser.flush()

    def wait_response(self, expect_cmd: int, timeout: float = 2.0) -> tuple:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                for msg_type, seq, payload in self.parser.feed(chunk):
                    if len(payload) >= 4 and payload[2] == expect_cmd:
                        return (msg_type, seq, payload)
            time.sleep(0.05)
        return None

    def _reply_ok(self, r, name: str) -> bool:
        if not r:
            print(f"[{name}] 无应答")
            return False
        msg_type, seq, payload = r
        result = payload[3] if len(payload) >= 4 else 0xFF
        if msg_type != MSG_ACK or result != 0:
            if len(payload) >= 6:
                exp_seq = payload[4] | (payload[5] << 8)
                print(f"[{name}] NACK result=0x{result:02X} expected_seq={exp_seq}")
                self._seq = exp_seq - 1
            else:
                print(f"[{name}] NACK result=0x{result:02X}")
            return False
        return True

    def hello(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_HELLO, seq))
        r = self.wait_response(CMD_HELLO, timeout=1.0)
        if self._reply_ok(r, "HELLO"):
            print(f"[HELLO] 成功 (seq={seq})")
            return True
        return False

    def begin(self, image_len: int, image_crc: int) -> bool:
        seq = self._next_seq()
        payload = struct.pack("<II", image_len, image_crc)
        self.send(encode_frame(CMD_BEGIN, seq, payload))
        r = self.wait_response(CMD_BEGIN, timeout=10.0)
        if self._reply_ok(r, "BEGIN"):
            print("[BEGIN] 成功，开始擦除（约 3-5 秒）")
            return True
        return False

    def data(self, offset: int, chunk: bytes) -> bool:
        seq = self._next_seq()
        payload = struct.pack("<I", offset) + chunk
        self.send(encode_frame(CMD_DATA, seq, payload))
        r = self.wait_response(CMD_DATA, timeout=1.0)
        if self._reply_ok(r, "DATA"):
            return True
        print(f"[DATA @{offset:#010x}] 失败")
        return False

    def end(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_END, seq))
        r = self.wait_response(CMD_END, timeout=5.0)
        if self._reply_ok(r, "END"):
            print("[END] 校验通过")
            return True
        return False

    def run(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_RUN, seq))
        r = self.wait_response(CMD_RUN, timeout=1.0)
        if r and self._reply_ok(r, "RUN"):
            print("[RUN] 已下发，设备即将运行新固件")
            return True
        return False

# ==================== 升级流程 ====================
def upgrade(port: str, bin_path: str):
    """通过 IAP 协议升级 APP"""
    print(f"[加载] {bin_path}")
    with open(bin_path, "rb") as f:
        image = f.read()
    image = image + b"\xff" * ((-len(image)) % 4)
    image_crc = crc32_zlib(image)
    print(f"[固件] 大小={len(image)}B  CRC32=0x{image_crc:08X}")

    client = IapClient(port)
    try:
        client.connect()

        # HELLO（带重试，兼容 APP 复位后需要时间）
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

        # BEGIN
        print(f"\n[阶段 2/5] BEGIN 擦除 APP 区...")
        if not client.begin(len(image), image_crc):
            print("[错误] BEGIN 失败")
            return False

        # DATA
        print(f"\n[阶段 3/5] DATA 写入固件...")
        chunk_size = MAX_PAYLOAD_LEN - 4
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

        # END
        print(f"\n[阶段 4/5] END 校验 CRC...")
        if not client.end():
            print("[错误] END 校验失败")
            return False

        # RUN
        print(f"\n[阶段 5/5] RUN 跳转 APP...")
        client.run()

        print("\n[IAP 升级成功！]")
        return True

    except Exception as e:
        print(f"[错误] {e}")
        return False
    finally:
        client.close()

# ==================== 主入口 ====================
def main():
    parser = argparse.ArgumentParser(
        description="STM32F407 IAP 固件烧录工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python flash.py --port COM7 --app APP_test.bin
  python flash.py --port COM7 --bootloader Bootloader_test.bin --app APP_test.bin

注意:
  - 使用 IAP 升级前，需先用 ST-Link 烧录 Bootloader
  - 或手动进入 bootloader 模式（短接 BOOT0 引脚）
        """
    )
    parser.add_argument("--port", required=True, help="串口号，如 COM7")
    parser.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    parser.add_argument("--bootloader", help="Bootloader 固件路径 (.bin)")
    parser.add_argument("--app", help="APP 固件路径 (.bin)")
    args = parser.parse_args()

    if not args.bootloader and not args.app:
        print("[错误] 请指定 --bootloader 或 --app")
        sys.exit(1)

    # 如果需要烧录 bootloader，先通过 ST-Link 烧录
    if args.bootloader:
        print(f"[ST-Link] 烧录 Bootloader: {args.bootloader}")
        import subprocess
        result = subprocess.run([
            "openocd", "-f", "interface/stlink.cfg",
            "-c", "transport select hla_swd",
            "-f", "target/stm32f4x.cfg",
            "-c", f"program {args.bootloader} 0x08000000 verify reset exit"
        ], capture_output=True, text=True, timeout=30)
        if "Verified OK" in result.stderr:
            print("[ST-Link] Bootloader 烧录成功！")
        else:
            print(f"[ST-Link] Bootloader 烧录失败: {result.stderr[-200:]}")
            sys.exit(1)

    # 如果需要烧录 APP，通过 IAP 升级
    if args.app:
        print()
        success = upgrade(args.port, args.app)
        sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
