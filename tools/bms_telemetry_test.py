#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""遥测链路验证（COM7 @115200）

持续发 0x12 心跳 → 设备以 200ms 周期推 MSG_TELEMETRY_FULL(0x01) 帧。
统计帧间隔、载荷长度；并确认会话期间文本行被抑制、停心跳 1s 后自动停流。
"""
import struct
import sys
import threading
import time

import serial

sys.path.insert(0, ".")
import bms_logic_test as T  # noqa: E402  (复用建帧/CRC)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
HEARTBEAT_PERIOD = 0.15
DURATION = 3.5
MSG_TELEMETRY_FULL = 0x01
EXPECT_PLEN = 454  # 固定区 52（含 SOC 1B）+ 有效位图 18 + 电芯 144*2 + 温度 48*2

stop = threading.Event()
frames = []      # (t, msg_type, plen)
texts = []       # (t, line)


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
            frames.append((time.time() - t0, frame[3], plen))


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.02)
    time.sleep(0.5)
    ser.reset_input_buffer()
    t0 = time.time()
    th = threading.Thread(target=reader, args=(ser, t0), daemon=True)
    th.start()

    print(f"发心跳 {DURATION}s ...（中途注入事件，验证会话期间文本行被抑制）")
    until = time.time() + DURATION
    t_start = time.time()
    n = 0
    events_sent = False
    while time.time() < until:
        frame, _ = T.build(T.MSG_HEARTBEAT, b"")
        ser.write(frame)
        ser.flush()
        n += 1
        if not events_sent and time.time() - t_start > 1.0:
            events_sent = True
            for cmd, pl, lbl in (
                (T.MSG_CAN_SELFTEST, bytes([1]), "selftest ON"),
                (T.MSG_CLEAR_ERRORS, b"", "clear errors"),
                (T.MSG_SET_INPUT, bytes([T.SIG_POWER_UP, 1]), "POWER_UP=1"),
            ):
                f, _ = T.build(cmd, pl)
                ser.write(f)
                ser.flush()
                print(f"   >> 注入 {lbl}")
                time.sleep(0.05)
        time.sleep(HEARTBEAT_PERIOD)
    print(f"心跳发出 {n} 个")

    print("停心跳，等 1.5s 看是否停流并恢复文本 ...")
    time.sleep(1.5)
    # 复位注入，让设备回到 INACTIVE
    f, _ = T.build(T.MSG_SET_INPUT, bytes([T.SIG_POWER_UP, 0]))
    ser.write(f)
    ser.flush()
    time.sleep(0.3)
    stop.set()
    ser.close()

    telem = [f for f in frames if f[1] == MSG_TELEMETRY_FULL]
    print(f"\n收到 TELEMETRY_FULL 帧: {len(telem)}")
    if len(telem) >= 2:
        iv = [(telem[i][0] - telem[i - 1][0]) * 1000 for i in range(1, len(telem))]
        print(f"  间隔 ms: min={min(iv):.0f} max={max(iv):.0f} "
              f"mean={sum(iv) / len(iv):.0f}")
    plens = {f[2] for f in telem}
    print(f"  载荷长度集合: {sorted(plens)} (期望 {EXPECT_PLEN})")

    ack = [f for f in frames if f[1] == T.MSG_ACK]
    print(f"收到 ACK 帧: {len(ack)}")

    during = [x for x in texts if x[0] < DURATION]
    after = [x for x in texts if x[0] >= DURATION]
    print(f"会话期间文本行: {len(during)} (期望 0)")
    for x in during[:6]:
        print(f"    ! {x[0]:.2f} {x[1]}")
    print(f"停流后文本行: {len(after)} (期望 >0，说明链路超时后恢复文本)")
    for x in after[:6]:
        print(f"    [ {x[0]:.2f} {x[1]}")


if __name__ == "__main__":
    main()
