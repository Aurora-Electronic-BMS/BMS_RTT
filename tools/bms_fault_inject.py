#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BMS 故障注入测试（COM7 @115200, B5 4D 协议）

不心跳 → 设备保持文本模式（[ST] 每秒一行，带 err/sm/vbat/minV/maxV/minT/maxT），
用 0x23 INJECT 直接注入 shunt/AFE/charge 故障，逐条核对保护分支是否按预期触发。

覆盖：
  S0 分流器超时      S1 SDC 断开        S2 分流器过流      S3 分流器过温
  S4 电压失配        S5 预充超时        S6 CHARGE 态
  S7 AFE 过压        S8 AFE 欠压        S9 AFE 过温(已知未实现)  S10 AFE 内部通信错误

注入 item 定义见 APP_test/Core/Inc/bms_io.h 的 BMS_FI_*。
"""
import re
import struct
import sys
import threading
import time

import serial

sys.path.insert(0, ".")
import bms_logic_test as T  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"

MSG_INJECT = 0x23

# ---- 注入 item（bms_io.h BMS_FI_*）----
FI_SHUNT_CURRENT = 0x00
FI_SHUNT_VBAT = 0x01
FI_SHUNT_VVEH = 0x02
FI_SHUNT_TEMP = 0x03
FI_AFE_CELL = 0x10
FI_AFE_AUX = 0x11
FI_AFE_PEC = 0x12
FI_CHARGE = 0x20

# ---- 错误位（state_machine.h TSErrorKind）----
ERR_OV = 0x002
ERR_UV = 0x004
ERR_OT = 0x008
ERR_SHUNT_TIMEOUT = 0x010
ERR_SHUNT_OC = 0x020
ERR_SHUNT_OT = 0x040
ERR_MISMATCH = 0x080
ERR_COMM = 0x100
ERR_PRECHARGE_TIMEOUT = 0x200

# ---- 状态（state_machine.h TSState）----
SM_INACTIVE, SM_ACTIVE, SM_PRECHARGE, SM_DISCHARGE, SM_ERROR, SM_PRE_HOLD, SM_CHARGE = range(7)

# 桩件：10 IC × 115 有效芯 × 3700.0 mV → Bat_Volt = 425500 mV
BENIGN_BAT = 425500

ST_LINE = re.compile(
    r"\[ST\] sm=(\d+) tgt=(\d+) err=0x([0-9A-Fa-f]+) air=0x([0-9A-Fa-f]+) "
    r"vbat=(-?\d+) vbat_sh=(-?\d+) i=(-?\d+) "
    r"minV=(\d+) maxV=(\d+) minT=(-?\d+) maxT=(-?\d+)"
)

stop = threading.Event()
LATEST = {}          # 最近一条 [ST] 解析结果
_seen_st = [0]


def reader(ser):
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
                for ln in bytes(buf).decode("utf-8", "replace").splitlines():
                    _handle_text(ln)
                buf.clear()
                break
            if i > 0:
                for ln in bytes(buf[:i]).decode("utf-8", "replace").splitlines():
                    _handle_text(ln)
                del buf[:i]
            if len(buf) < 8:
                break
            plen = struct.unpack_from("<H", buf, 6)[0]
            total = 2 + 6 + plen + 2
            if len(buf) < total:
                break
            frame = bytes(buf[:total])
            del buf[:total]
            if frame[3] == T.MSG_ACK:
                cseq, ccmd, res, reason = struct.unpack_from("<HBBB", frame, 8)
                if res != 0:
                    print(f"    ACK! cmd=0x{ccmd:02X} result={T.ACK_NAMES.get(res, res)} reason={reason}")


def _handle_text(line):
    line = line.strip()
    if not line:
        return
    m = ST_LINE.search(line)
    if m:
        LATEST.update(
            sm=int(m.group(1)), tgt=int(m.group(2)), err=int(m.group(3), 16),
            air=int(m.group(4), 16), vbat=int(m.group(5)), vbat_sh=int(m.group(6)),
            i=int(m.group(7)), minV=int(m.group(8)), maxV=int(m.group(9)),
            minT=int(m.group(10)), maxT=int(m.group(11)), t=time.time(),
        )
        _seen_st[0] += 1
        print(f"    [ST] sm={LATEST['sm']} err=0x{LATEST['err']:X} "
              f"i={LATEST['i']} vbat_sh={LATEST['vbat_sh']} "
              f"minV={LATEST['minV']} minT={LATEST['minT']} maxT={LATEST['maxT']}")
        return
    if "[SM]" in line or "FAIL" in line:
        print(f"    {line}")


def send(ser, cmd, payload=b""):
    frame, _ = T.build(cmd, payload)
    ser.write(frame)
    ser.flush()


def inject(ser, item, value):
    send(ser, MSG_INJECT, bytes([item]) + struct.pack("<i", value))


def set_input(ser, sig, value):
    send(ser, T.MSG_SET_INPUT, bytes([sig, value]))


def selftest(ser, on):
    send(ser, T.MSG_CAN_SELFTEST, bytes([1 if on else 0]))


def clear_errors(ser):
    send(ser, T.MSG_CLEAR_ERRORS, b"")


BENIGN = [
    (FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_VVEH, BENIGN_BAT),
    (FI_SHUNT_CURRENT, 0), (FI_SHUNT_TEMP, 0),
    (FI_AFE_CELL, -1), (FI_AFE_AUX, -1), (FI_AFE_PEC, -1),
    (FI_CHARGE, 0),
]


def recover(ser, timeout=4.0):
    """撤销全部注入、复位输入、清故障，等设备回到无错非 ERROR 态。"""
    set_input(ser, T.SIG_POWER_UP, 0)
    set_input(ser, T.SIG_SDC_VOLTAGE, 1)
    selftest(ser, False)
    t0 = time.time()
    while time.time() - t0 < timeout:
        for it, v in BENIGN:
            inject(ser, it, v)
        if not (time.time() - t0 < 0.6):
            clear_errors(ser)
        e = LATEST.get("err")
        if e is not None and (e & ~0x1) == 0 and LATEST.get("sm") != SM_ERROR:
            return True
        time.sleep(0.15)
    print(f"    recover 超时: err=0x{LATEST.get('err', -1):X} sm={LATEST.get('sm')}")
    return False


def hold(ser, injects, pred, timeout, desc, expect_absent=False):
    """循环注入并观察 pred(LATEST)，直到成立(或整个窗口都不成立)。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for it, v in injects:
            inject(ser, it, v)
        got = pred(LATEST)
        if expect_absent:
            if got:
                print(f"  FAIL {desc}: 不应出现但出现了 (err=0x{LATEST.get('err', 0):X})")
                return False
        elif got:
            print(f"  PASS {desc}  ({time.time() - t0:.2f}s, "
                  f"sm={LATEST.get('sm')} err=0x{LATEST.get('err', 0):X})")
            return True
        time.sleep(0.15)
    if expect_absent:
        print(f"  PASS {desc}  (窗口内未出现，err=0x{LATEST.get('err', 0):X})")
        return True
    print(f"  FAIL {desc}  超时 (sm={LATEST.get('sm')} err=0x{LATEST.get('err', -1):X})")
    return False


results = []


def record(ok, name):
    results.append((name, ok))
    print(f"  --> {'PASS' if ok else 'FAIL'}  {name}")


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.05)
    time.sleep(1.0)
    ser.reset_input_buffer()
    threading.Thread(target=reader, args=(ser,), daemon=True).start()

    # 等第一条 [ST]（设备每秒打一行）
    t0 = time.time()
    while _seen_st[0] == 0 and time.time() - t0 < 4:
        time.sleep(0.1)
    print(f"设备上线，首条 [ST] 已收\n")

    # ===== S0 分流器超时：不注入 + 自测关，last_message 变陈旧 → ~1s 触发 =====
    print("=== S0 分流器超时（停止注入，靠时间戳陈旧触发）===")
    recover(ser)
    # recover 会刷 last_message，这里静默等待它变陈旧
    ok = hold(ser, [], lambda st: (st.get("err", 0) & ERR_SHUNT_TIMEOUT) != 0,
              4.0, "SHUNT_TIMEOUT(err&0x10)")
    record(ok, "S0 分流器超时")

    # ===== S1 SDC 断开：ACTIVE 中拉开 SDC → DISCHARGE =====
    print("\n=== S1 SDC 断开（自测 ON 灌正常值，先到 ACTIVE）===")
    recover(ser)
    selftest(ser, True)
    set_input(ser, T.SIG_POWER_UP, 1)
    ok1 = hold(ser, [], lambda st: st.get("sm") == SM_ACTIVE, 4.0, "先进入 ACTIVE")
    set_input(ser, T.SIG_SDC_VOLTAGE, 0)
    ok2 = hold(ser, [], lambda st: st.get("sm") in (SM_DISCHARGE, SM_INACTIVE), 3.0,
               "SDC 断开 → DISCHARGE")
    record(ok1 and ok2, "S1 SDC 断开")
    set_input(ser, T.SIG_SDC_VOLTAGE, 1)
    set_input(ser, T.SIG_POWER_UP, 0)

    # ===== S2 分流器过流 =====
    print("\n=== S2 分流器过流（i=20000 > 15000）===")
    recover(ser)
    ok = hold(ser, [(FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_CURRENT, 20000)],
              lambda st: (st.get("err", 0) & ERR_SHUNT_OC) != 0,
              4.0, "SHUNT_OVERCURRENT(err&0x20)")
    record(ok, "S2 分流器过流")

    # ===== S3 分流器过温 =====
    print("\n=== S3 分流器过温（busbar=1200 > 1000）===")
    recover(ser)
    ok = hold(ser, [(FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_CURRENT, 0), (FI_SHUNT_TEMP, 1200)],
              lambda st: (st.get("err", 0) & ERR_SHUNT_OT) != 0,
              4.0, "SHUNT_OVERTEMP(err&0x40)")
    record(ok, "S3 分流器过温")

    # ===== S4 电压失配 =====
    print("\n=== S4 电压失配（|425500-400000|=25500 > 10000）===")
    recover(ser)
    ok = hold(ser, [(FI_SHUNT_VBAT, 400000), (FI_SHUNT_CURRENT, 0), (FI_SHUNT_TEMP, 0)],
              lambda st: (st.get("err", 0) & ERR_MISMATCH) != 0,
              4.0, "VOLTAGE_MISMATCH(err&0x80)")
    record(ok, "S4 电压失配")

    # ===== S5 预充超时：车辆侧电压太低，5s 内到不了 0.95·vbat =====
    print("\n=== S5 预充超时（vveh=100 达不到 0.95·vbat，5s 后超时）===")
    recover(ser)
    set_input(ser, T.SIG_POWER_UP, 1)
    ok = hold(ser, [(FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_VVEH, 100), (FI_SHUNT_CURRENT, 0)],
              lambda st: (st.get("err", 0) & ERR_PRECHARGE_TIMEOUT) != 0,
              8.0, "PRECHARGE_TIMEOUT(err&0x200)")
    record(ok, "S5 预充超时")
    set_input(ser, T.SIG_POWER_UP, 0)

    # ===== S6 CHARGE 态：注入 charge_mode + 车辆侧略高于电池 =====
    print("\n=== S6 CHARGE 态（charge_mode=1, vveh>vbat）===")
    recover(ser)
    set_input(ser, T.SIG_POWER_UP, 1)
    ok = hold(ser, [(FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_VVEH, BENIGN_BAT + 500),
                    (FI_SHUNT_CURRENT, 0), (FI_CHARGE, 1)],
              lambda st: st.get("sm") == SM_CHARGE,
              6.0, "进入 CHARGE(sm=6)")
    record(ok, "S6 CHARGE 态")
    set_input(ser, T.SIG_POWER_UP, 0)

    # ===== S7 AFE 过压 =====
    print("\n=== S7 AFE 过压（cell_raw=43000 → 4300mV > 4200mV）===")
    recover(ser)
    bat7 = 115 * (43000 // 10)
    ok = hold(ser, [(FI_AFE_CELL, 43000), (FI_SHUNT_VBAT, bat7), (FI_SHUNT_CURRENT, 0)],
              lambda st: (st.get("err", 0) & ERR_OV) != 0,
              4.0, "SLAVE_OVERVOLT(err&0x2)")
    record(ok, "S7 AFE 过压")

    # ===== S8 AFE 欠压 =====
    print("\n=== S8 AFE 欠压（cell_raw=28000 → 2800mV < 3000mV）===")
    recover(ser)
    bat8 = 115 * (28000 // 10)
    ok = hold(ser, [(FI_AFE_CELL, 28000), (FI_SHUNT_VBAT, bat8), (FI_SHUNT_CURRENT, 0)],
              lambda st: (st.get("err", 0) & ERR_UV) != 0,
              4.0, "SLAVE_UNDERVOLT(err&0x4)")
    record(ok, "S8 AFE 欠压")

    # ===== S9 AFE 过温（预期：不报，因 slave_monitor.c 该分支被注释）=====
    print("\n=== S9 AFE 过温（aux=6500 → ~55℃；预期不报，分支被注释）===")
    recover(ser)
    hold(ser, [(FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_CURRENT, 0), (FI_AFE_AUX, 6500)],
         lambda st: (st.get("err", 0) & ERR_OT) != 0,
         2.5, "SLAVE_OVERTEMP(err&0x8) 未出现", expect_absent=True)
    ok = LATEST.get("maxT", 0) > 50
    note = "注入生效但未报错 → 确认过温保护未实现" if ok else "注入未生效"
    print(f"    实测 maxT={LATEST.get('maxT')}℃（注入 ~55℃；{note}）")
    record(ok, "S9 AFE 过温注入生效（未报错=已知缺口）")

    # ===== S10 AFE 内部通信错误：PEC 坏 + 电流 < -1000mA =====
    print("\n=== S10 AFE 内部通信错误（PEC=255 且 i=-2000 < -1000）===")
    recover(ser)
    ok = hold(ser, [(FI_AFE_PEC, 255), (FI_SHUNT_VBAT, BENIGN_BAT), (FI_SHUNT_CURRENT, -2000)],
              lambda st: (st.get("err", 0) & ERR_COMM) != 0,
              4.0, "INTERNAL_COMM_ERROR(err&0x100)")
    record(ok, "S10 AFE 内部通信错误")

    recover(ser)
    stop.set()
    ser.close()

    npass = sum(1 for _, ok in results if ok)
    print(f"\n=== 汇总: {npass}/{len(results)} 通过 ===")
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")


if __name__ == "__main__":
    main()
