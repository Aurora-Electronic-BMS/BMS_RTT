#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""长稳/死锁压测（COM7 @115200）

持续心跳 + CAN 回环自测 + 每 2s 翻转 POWER_UP，跑 DURATION 秒。
期望：遥测帧以 ~200ms 稳定输出、无长空档、会话期间无文本行（锁没卡死）。
"""
import struct
import sys
import threading
import time

import serial

sys.path.insert(0, ".")
import bms_logic_test as T  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 60
MSG_TELEMETRY_FULL = 0x01

stop = threading.Event()
frames = []
texts = []


def reader(ser, t0):
    buf = bytearray()
    while not stop.is_set():
        try:
            chunk = ser.read(512)
        except Exception:
            break
        if not chunk:
            continue
        buf += chunk
        while True:
            i = buf.find(b"\xB5\x4D")
            if i < 0:
                for ln in bytes(buf).decode("utf-8", "replace").splitlines():
                    if ln.strip():
                        texts.append((time.time() - t0, ln.strip()))
                buf.clear()
                break
            if i > 0:
                for ln in bytes(buf[:i]).decode("utf-8", "replace").splitlines():
                    if ln.strip():
                        texts.append((time.time() - t0, ln.strip()))
                del buf[:i]
            if len(buf) < 8:
                break
            plen = struct.unpack_from("<H", buf, 6)[0]
            total = 2 + 6 + plen + 2
            if len(buf) < total:
                break
            frame = bytes(buf[:total])
            del buf[:total]
            if frame[3] == MSG_TELEMETRY_FULL:
                frames.append(time.time() - t0)


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.02)
    time.sleep(0.5)
    ser.reset_input_buffer()
    t0 = time.time()
    threading.Thread(target=reader, args=(ser, t0), daemon=True).start()

    f, _ = T.build(T.MSG_CAN_SELFTEST, bytes([1]))
    ser.write(f)
    ser.flush()
    print(f"selftest ON；开始 {DURATION}s 心跳+翻转压测 ...")

    until = time.time() + DURATION
    lvl = 0
    n = 0
    last_toggle = 0.0
    while time.time() < until:
        f, _ = T.build(T.MSG_HEARTBEAT, b"")
        ser.write(f)
        ser.flush()
        n += 1
        now = time.time()
        if now - last_toggle >= 2.0:
            last_toggle = now
            lvl ^= 1
            f, _ = T.build(T.MSG_SET_INPUT, bytes([T.SIG_POWER_UP, lvl]))
            ser.write(f)
            ser.flush()
        time.sleep(0.15)

    print(f"心跳 {n} 个，遥测帧 {len(frames)} 个")
    if len(frames) >= 2:
        iv = [(frames[i] - frames[i - 1]) * 1000 for i in range(1, len(frames))]
        iv_sorted = sorted(iv)
        p99 = iv_sorted[int(len(iv_sorted) * 0.99)]
        print(f"  帧间隔 ms: min={min(iv):.0f} mean={sum(iv) / len(iv):.0f} "
              f"p99={p99:.0f} max={max(iv):.0f}")
        print(f"  最大空档 {max(iv):.0f}ms（若锁卡死会出现数百 ms~秒级空档）")
    print(f"会话期间文本行: {len(texts)} (期望 0)")
    for x in texts[:10]:
        print(f"    ! {x[0]:.2f} {x[1]}")
    stop.set()
    ser.close()


if __name__ == "__main__":
    main()
