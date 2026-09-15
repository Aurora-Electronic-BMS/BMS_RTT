#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BMS 逻辑验证（COM7 @115200, B5 4D 协议）

流程：CAN 回环自测 ON → 清故障 → 注入 POWER_UP=1 → 观察
PRECHARGE(air=0x6) → PRE_HOLD(air=0x7) → ACTIVE(air=0x3) → HV_ACTIVE=1，
再注入 POWER_UP=0 → DISCHARGE(air=0x0) → INACTIVE(air=0x0)。

后台线程持续打印设备文本行([OUT]/[SM]/[ST]/[INJ])，主线程序列发命令并收 ACK。
"""
import struct
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = 115200

VER = 1
MSG_ACK = 0x02
MSG_SET_TARGET = 0x10
MSG_HEARTBEAT = 0x12
MSG_CLEAR_ERRORS = 0x14
MSG_SET_INPUT = 0x20
MSG_DUMP_IO = 0x21
MSG_CAN_SELFTEST = 0x22

SIG_POWER_UP = 0
SIG_CHARGE_ENABLE = 1
SIG_SDC_VOLTAGE = 2

ACK_NAMES = {0: "OK", 2: "BAD_LENGTH", 3: "UNKNOWN_CMD", 4: "REJECTED"}


def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


_tx_seq = [0]


def build(cmd, payload=b""):
    _tx_seq[0] = (_tx_seq[0] + 1) & 0xFFFF
    seq = _tx_seq[0]
    body = struct.pack("<BBHH", VER, cmd, seq, len(payload)) + payload
    crc = crc16_modbus(body)
    return b"\xB5\x4D" + body + struct.pack("<H", crc), seq


stop = threading.Event()
T0 = time.time()


def stamp():
    return f"[{time.time() - T0:7.3f}]"


def reader(ser):
    """持续解析设备上行：打印文本行 + ACK。"""
    buf = bytearray()
    while not stop.is_set():
        try:
            chunk = ser.read(256)
        except Exception:
            break
        if not chunk:
            continue
        buf += chunk
        while True:
            i = buf.find(b"\xB5\x4D")
            if i < 0:
                # 无帧头：把可打印的文本行打出来
                text = bytes(buf).decode("utf-8", "replace")
                for line in text.splitlines():
                    if line.strip():
                        print(f"{stamp()} [dev] {line.strip()}")
                buf.clear()
                break
            if i > 0:
                text = bytes(buf[:i]).decode("utf-8", "replace")
                for line in text.splitlines():
                    if line.strip():
                        print(f"{stamp()} [dev] {line.strip()}")
                del buf[:i]
            if len(buf) < 8:
                break
            plen = struct.unpack_from("<H", buf, 6)[0]
            total = 2 + 6 + plen + 2
            if len(buf) < total:
                break
            frame = bytes(buf[:total])
            del buf[:total]
            cmd = frame[3]
            if cmd == MSG_ACK:
                cseq, ccmd, res, reason = struct.unpack_from("<HBBB", frame, 8)
                print(f"{stamp()} ACK seq={cseq} cmd=0x{ccmd:02X} "
                      f"result={ACK_NAMES.get(res, res)}({res}) reason={reason}")


def send(ser, cmd, payload=b"", label=""):
    frame, seq = build(cmd, payload)
    print(f">> {label or hex(cmd)} (cmd=0x{cmd:02X} seq={seq})")
    ser.write(frame)
    ser.flush()
    time.sleep(0.25)


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(1.5)
    ser.reset_input_buffer()

    t = threading.Thread(target=reader, args=(ser,), daemon=True)
    t.start()

    print("=== 1. CAN 回环自测 ON（合成 0x521-0x525 灌入 shunt_data） ===")
    send(ser, MSG_CAN_SELFTEST, bytes([1]), "CAN selftest ON")
    time.sleep(0.8)

    print("=== 2. 清故障 ===")
    send(ser, MSG_CLEAR_ERRORS, b"", "clear errors")
    send(ser, MSG_DUMP_IO, b"", "dump io")

    print("=== 3. 注入 POWER_UP=1（INACTIVE→PRECHARGE→PRE_HOLD→ACTIVE） ===")
    send(ser, MSG_SET_INPUT, bytes([SIG_POWER_UP, 1]), "POWER_UP=1")
    time.sleep(1.6)

    print("=== 4. 注入 POWER_UP=0（ACTIVE→DISCHARGE→INACTIVE） ===")
    send(ser, MSG_SET_INPUT, bytes([SIG_POWER_UP, 0]), "POWER_UP=0")
    time.sleep(1.2)

    print("=== 5. CAN 回环自测 OFF ===")
    send(ser, MSG_CAN_SELFTEST, bytes([0]), "CAN selftest OFF")

    time.sleep(0.5)
    stop.set()
    ser.close()
    print("=== done ===")


if __name__ == "__main__":
    main()
