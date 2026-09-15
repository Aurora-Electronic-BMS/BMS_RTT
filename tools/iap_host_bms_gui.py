#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32F407 IAP 固件烧录工具（带 GUI）
支持：
  1. 通过 ST-Link 烧录 Bootloader
  2. 通过 IAP 协议升级 APP
"""
import argparse
import os
import struct
import sys
import time
import zlib
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

try:
    import serial
except ImportError:
    sys.exit("错误：缺少 pyserial，请先执行: pip install pyserial")

# ==================== IAP 协议常量 ====================
MAGIC = b"\xB5\x4D"
VERSION = 1
MAX_PAYLOAD_LEN = 256
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
        self.log_callback = None

    def set_log(self, callback):
        self.log_callback = callback

    def log(self, msg: str):
        if self.log_callback:
            self.log_callback(msg)

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFFFF
        return self._seq

    def connect(self):
        self.ser = serial.Serial(self.port, self.baudrate, timeout=0.5)
        return True

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
            self.log(f"[{name}] 无应答")
            return False
        msg_type, seq, payload = r
        result = payload[3] if len(payload) >= 4 else 0xFF
        if msg_type != MSG_ACK or result != 0:
            if len(payload) >= 6:
                exp_seq = payload[4] | (payload[5] << 8)
                self.log(f"[{name}] NACK result=0x{result:02X} expected_seq={exp_seq}")
                self._seq = exp_seq - 1
            else:
                self.log(f"[{name}] NACK result=0x{result:02X}")
            return False
        return True

    def hello(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_HELLO, seq))
        r = self.wait_response(CMD_HELLO, timeout=1.0)
        if self._reply_ok(r, "HELLO"):
            self.log(f"  [OK] HELLO seq={seq}")
            return True
        return False

    def begin(self, image_len: int, image_crc: int) -> bool:
        seq = self._next_seq()
        payload = struct.pack("<II", image_len, image_crc)
        self.send(encode_frame(CMD_BEGIN, seq, payload))
        r = self.wait_response(CMD_BEGIN, timeout=10.0)
        if self._reply_ok(r, "BEGIN"):
            self.log(f"  [OK] BEGIN seq={seq}")
            return True
        return False

    def data(self, offset: int, chunk: bytes) -> bool:
        seq = self._next_seq()
        payload = struct.pack("<I", offset) + chunk
        self.send(encode_frame(CMD_DATA, seq, payload))
        # 非阻塞读取，快速轮询响应
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            if self.ser.in_waiting:
                for msg_type, seq, payload in self.parser.feed(self.ser.read(self.ser.in_waiting)):
                    if len(payload) >= 4 and payload[2] == CMD_DATA:
                        result = payload[3] if len(payload) >= 4 else 0xFF
                        if msg_type == MSG_ACK and result == 0:
                            return True
                        if len(payload) >= 6:
                            exp_seq = payload[4] | (payload[5] << 8)
                            self.log(f"[DATA] NACK result=0x{result:02X} expected_seq={exp_seq}")
                            self._seq = exp_seq - 1
                        else:
                            self.log(f"[DATA] NACK result=0x{result:02X}")
                        return False
            time.sleep(0.001)  # 1ms yield
        self.log(f"  [FAIL] DATA @{offset:#010x} 超时")
        return False

    def end(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_END, seq))
        r = self.wait_response(CMD_END, timeout=5.0)
        if self._reply_ok(r, "END"):
            self.log(f"  [OK] END seq={seq}")
            return True
        return False

    def run(self) -> bool:
        seq = self._next_seq()
        self.send(encode_frame(CMD_RUN, seq))
        r = self.wait_response(CMD_RUN, timeout=1.0)
        if r and self._reply_ok(r, "RUN"):
            self.log(f"  [OK] RUN seq={seq}")
            return True
        return False

# ==================== 升级流程 ====================
def do_upgrade(port, bin_path, log_callback, progress_callback):
    with open(bin_path, "rb") as f:
        image = f.read()
    image = image + b"\xff" * ((-len(image)) % 4)
    image_crc = crc32_zlib(image)
    log_callback(f"[固件] 大小={len(image)}B  CRC32=0x{image_crc:08X}")

    client = IapClient(port)
    client.set_log(log_callback)
    try:
        client.connect()
        log_callback(f"[连接] {port} @ {client.baudrate}")

        # HELLO
        log_callback("[阶段 1/5] HELLO 握手...")
        for i in range(10):
            if client.hello():
                break
            if i == 0:
                log_callback("  APP 未响应，发送升级触发帧...")
                client.send(_APP_TRIGGER_FRAME)
                log_callback("  等待 APP 软复位（约 1s）...")
                time.sleep(1.0)
                client.parser._buffer.clear()
            time.sleep(0.3)
        else:
            log_callback("  [FAIL] 请确认设备已进入 bootloader")
            return False

        # BEGIN
        log_callback("[阶段 2/5] BEGIN 擦除 APP 区...")
        if not client.begin(len(image), image_crc):
            log_callback("  [FAIL]")
            return False
        log_callback("  [OK] 擦除完成")
        progress_callback(20)

        # DATA
        log_callback("[阶段 3/5] DATA 写入固件...")
        chunk_size = 252  # 256 - 4(offset)
        total = len(image)
        for off in range(0, total, chunk_size):
            chunk = image[off:off+chunk_size]
            if not client.data(off, chunk):
                log_callback(f"  [FAIL] 写入失败 @ 0x{off:08x}")
                return False
            pct = 20 + 60 * off // total
            progress_callback(pct)
            if off % (chunk_size * 50) == 0:
                log_callback(f"  进度: {off}/{total} ({pct}%)")
        log_callback(f"  [OK] 写入完成 ({total}B)")

        # END
        log_callback("[阶段 4/5] END 校验 CRC...")
        if not client.end():
            log_callback("  [FAIL] 校验失败")
            return False
        log_callback("  [OK] 校验通过")
        progress_callback(85)

        # RUN
        log_callback("[阶段 5/5] RUN 跳转 APP...")
        client.run()
        log_callback("  [OK] 已下发，设备即将运行新固件")
        progress_callback(100)

        log_callback("\n[IAP 升级成功！]")
        return True

    except Exception as e:
        log_callback(f"[错误] {e}")
        return False
    finally:
        client.close()

# ==================== ST-Link 烧录 ====================
def flash_via_stlink(bin_path, address, log_callback):
    import subprocess
    log_callback(f"[ST-Link] 烧录 {bin_path} 到 0x{address:08X}...")
    result = subprocess.run([
        "openocd", "-f", "interface/stlink.cfg",
        "-c", "transport select hla_swd",
        "-f", "target/stm32f4x.cfg",
        "-c", f"program {bin_path} 0x{address:08X} verify reset exit"
    ], capture_output=True, text=True, timeout=60)
    if "Verified OK" in result.stderr:
        log_callback(f"[ST-Link] 烧录成功！")
        return True
    else:
        log_callback(f"[ST-Link] 烧录失败")
        log_callback(result.stderr[-300:])
        return False

# ==================== GUI ====================
class IapGui:
    def __init__(self, root):
        self.root = root
        self.root.title("STM32F407 IAP 固件烧录工具")
        self.root.geometry("700x600")
        self.root.resizable(True, True)
        self.upgrading = False

        self._build_ui()
        self._refresh_ports()

    def _build_ui(self):
        # === Tab 控件 ===
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=10, pady=5)

        # IAP 升级 Tab
        self.iap_tab = ttk.Frame(self.notebook)
        self.notebook.add(self.iap_tab, text="IAP 升级 APP")

        # ST-Link 烧录 Tab
        self.stlink_tab = ttk.Frame(self.notebook)
        self.notebook.add(self.stlink_tab, text="ST-Link 烧录")

        self._build_iap_tab()
        self._build_stlink_tab()

    def _build_iap_tab(self):
        # 配置区
        cfg_frame = ttk.LabelFrame(self.iap_tab, text="配置", padding=10)
        cfg_frame.pack(fill="x", padx=10, pady=5)

        # 串口选择
        ttk.Label(cfg_frame, text="串口号:").grid(row=0, column=0, sticky="w", pady=5)
        self.port_var = tk.StringVar(value="COM7")
        self.port_entry = ttk.Entry(cfg_frame, textvariable=self.port_var, width=15)
        self.port_entry.grid(row=0, column=1, padx=5)
        ttk.Button(cfg_frame, text="刷新", command=self._refresh_ports).grid(row=0, column=2, padx=5)

        # 波特率
        ttk.Label(cfg_frame, text="波特率:").grid(row=0, column=3, sticky="w", padx=(20,0), pady=5)
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(cfg_frame, textvariable=self.baud_var,
                     values=["9600","115200","230400","460800"],
                     width=10, state="readonly").grid(row=0, column=4, padx=5)

        # 固件文件
        ttk.Label(cfg_frame, text="固件:").grid(row=1, column=0, sticky="w", pady=5)
        self.bin_var = tk.StringVar()
        ttk.Entry(cfg_frame, textvariable=self.bin_var, width=45).grid(row=1, column=1, padx=5, sticky="ew", columnspan=2)
        ttk.Button(cfg_frame, text="选择文件", command=self._select_bin).grid(row=1, column=3, padx=5)
        cfg_frame.columnconfigure(1, weight=1)

        # 操作按钮
        btn_frame = ttk.Frame(self.iap_tab, padding=10)
        btn_frame.pack(fill="x", padx=10)

        self.start_btn = ttk.Button(btn_frame, text="开始升级", command=self._start_upgrade, width=15)
        self.start_btn.pack(side="left", padx=5)

        self.stop_btn = ttk.Button(btn_frame, text="停止", command=self._stop_upgrade, width=10, state="disabled")
        self.stop_btn.pack(side="left", padx=5)

        # 进度条
        prog_frame = ttk.LabelFrame(self.iap_tab, text="进度", padding=5)
        prog_frame.pack(fill="x", padx=10, pady=5)
        self.progress = ttk.Progressbar(prog_frame, length=600, mode="determinate")
        self.progress.pack(padx=5, pady=5)
        self.progress_label = ttk.Label(prog_frame, text="就绪")
        self.progress_label.pack()

        # 日志区
        log_frame = ttk.LabelFrame(self.iap_tab, text="日志", padding=5)
        log_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, width=80, height=15, wrap="word")
        self.log_text.pack(fill="both", expand=True)

        # 状态栏
        self.status_var = tk.StringVar(value="就绪")
        ttk.Label(self.iap_tab, textvariable=self.status_var, relief="sunken", padding=5).pack(fill="x")

    def _build_stlink_tab(self):
        # 配置区
        cfg_frame = ttk.LabelFrame(self.stlink_tab, text="ST-Link 配置", padding=10)
        cfg_frame.pack(fill="x", padx=10, pady=5)

        # 固件文件
        ttk.Label(cfg_frame, text="固件:").grid(row=0, column=0, sticky="w", pady=5)
        self.stbin_var = tk.StringVar()
        ttk.Entry(cfg_frame, textvariable=self.stbin_var, width=45).grid(row=0, column=1, padx=5, sticky="ew")
        ttk.Button(cfg_frame, text="选择文件", command=self._select_stbin).grid(row=0, column=2, padx=5)

        # 烧录地址
        ttk.Label(cfg_frame, text="地址:").grid(row=1, column=0, sticky="w", pady=5)
        self.addr_var = tk.StringVar(value="0x08000000")
        ttk.Entry(cfg_frame, textvariable=self.addr_var, width=15).grid(row=1, column=1, padx=5)
        ttk.Label(cfg_frame, text="(默认: 0x08000000 = Bootloader)").grid(row=1, column=2, sticky="w", padx=5)

        cfg_frame.columnconfigure(1, weight=1)

        # 操作按钮
        btn_frame = ttk.Frame(self.stlink_tab, padding=10)
        btn_frame.pack(fill="x", padx=10)

        self.stflash_btn = ttk.Button(btn_frame, text="烧录到 Flash", command=self._start_stflash, width=15)
        self.stflash_btn.pack(side="left", padx=5)

        # 进度条
        prog_frame = ttk.LabelFrame(self.stlink_tab, text="进度", padding=5)
        prog_frame.pack(fill="x", padx=10, pady=5)
        self.stprogress = ttk.Progressbar(prog_frame, length=600, mode="determinate")
        self.stprogress.pack(padx=5, pady=5)
        self.stprogress_label = ttk.Label(prog_frame, text="就绪")
        self.stprogress_label.pack()

        # 日志区
        log_frame = ttk.LabelFrame(self.stlink_tab, text="日志", padding=5)
        log_frame.pack(fill="both", expand=True, padx=10, pady=5)
        self.stlog_text = scrolledtext.ScrolledText(log_frame, width=80, height=15, wrap="word")
        self.stlog_text.pack(fill="both", expand=True)

    def _refresh_ports(self):
        try:
            import serial.tools.list_ports as lp
            ports = [p.device for p in lp.comports()]
            if ports:
                self.port_var.set(ports[0])
            self.log(f"[刷新] 可用串口: {', '.join(ports) if ports else '无'}")
        except Exception as e:
            self.log(f"[刷新] 失败: {e}")

    def _select_bin(self):
        path = filedialog.askopenfilename(
            title="选择 APP 固件文件",
            filetypes=[("BIN 文件", "*.bin"), ("所有文件", "*.*")]
        )
        if path:
            self.bin_var.set(path)
            self.log(f"[文件] {path}")

    def _select_stbin(self):
        path = filedialog.askopenfilename(
            title="选择固件文件",
            filetypes=[("BIN 文件", "*.bin"), ("所有文件", "*.*")]
        )
        if path:
            self.stbin_var.set(path)
            self.stlog(f"[文件] {path}")

    def _start_upgrade(self):
        if self.upgrading:
            return
        bin_path = self.bin_var.get().strip()
        port = self.port_var.get().strip()
        if not bin_path:
            messagebox.showerror("错误", "请先选择固件文件")
            return
        if not port:
            messagebox.showerror("错误", "请指定串口号")
            return
        if not os.path.exists(bin_path):
            messagebox.showerror("错误", f"文件不存在: {bin_path}")
            return

        self.upgrading = True
        self.start_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self.progress["value"] = 0
        self.status_var.set("升级中...")
        self.log("=" * 50)
        self.log(f"开始升级: 串口={port}  固件={bin_path}")

        threading.Thread(target=self._run_upgrade_thread, args=(port, bin_path), daemon=True).start()

    def _start_stflash(self):
        bin_path = self.stbin_var.get().strip()
        if not bin_path:
            messagebox.showerror("错误", "请先选择固件文件")
            return
        if not os.path.exists(bin_path):
            messagebox.showerror("错误", f"文件不存在: {bin_path}")
            return

        try:
            address = int(self.addr_var.get().strip(), 0)
        except ValueError:
            messagebox.showerror("错误", "地址格式错误")
            return

        self.stlog(f"[烧录] {bin_path} -> 0x{address:08X}")
        self.stprogress["value"] = 0
        self.stprogress_label.config(text="烧录中...")
        self.stflash_btn.config(state="disabled")

        def worker():
            success = flash_via_stlink(bin_path, address, self.stlog)
            self.stprogress["value"] = 100 if success else 50
            self.stprogress_label.config(text="完成" if success else "失败")
            self.stflash_btn.config(state="normal")

        threading.Thread(target=worker, daemon=True).start()

    def _stop_upgrade(self):
        self.upgrading = False
        self.log("[用户] 已停止")
        self._upgrade_finished()

    def _set_progress(self, pct):
        self.progress["value"] = pct
        self.progress_label.config(text=f"写入中... {pct}%")

    def _run_upgrade_thread(self, port, bin_path):
        try:
            def log(msg):
                self.log(msg)
            def progress(pct):
                self.root.after(0, lambda p=pct: self._set_progress(p))
            success = do_upgrade(port, bin_path, log, progress)
            self.root.after(0, lambda: self.status_var.set("成功" if success else "失败"))
        finally:
            self.root.after(0, self._upgrade_finished)

    def _upgrade_finished(self):
        self.upgrading = False
        self.start_btn.config(state="normal")
        self.stop_btn.config(state="disabled")
        if self.progress["value"] >= 100:
            self.progress_label.config(text="[OK] 升级完成")
        else:
            self.progress_label.config(text="[X] 已取消")

    def log(self, msg):
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.insert("end", f"[{timestamp}] {msg}\n")
        self.log_text.see("end")

    def stlog(self, msg):
        timestamp = time.strftime("%H:%M:%S")
        self.stlog_text.insert("end", f"[{timestamp}] {msg}\n")
        self.stlog_text.see("end")

if __name__ == "__main__":
    root = tk.Tk()
    app = IapGui(root)
    root.mainloop()
