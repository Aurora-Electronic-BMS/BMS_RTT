from __future__ import annotations

import queue
import struct
import time
import zlib

from PyQt6.QtCore import QThread, pyqtSignal
import serial
from serial.tools import list_ports

from .protocol import (
    BOOT_APP_START,
    BOOT_DATA_SIZE,
    BOOT_MAX_IMAGE_SIZE,
    BmsConfiguration,
    BootResult,
    FrameParser,
    MsgType,
    build_boot_begin,
    build_boot_data,
    build_boot_end,
    build_boot_hello,
    build_boot_run,
    build_clear_errors,
    build_enter_bootloader,
    build_heartbeat,
    build_get_configuration,
    build_set_manual_air,
    build_set_configuration,
    build_set_target_state,
    build_software_reset,
    build_update_soc_from_ocv,
    decode_ack,
    decode_boot_response,
    decode_configuration,
    decode_telemetry,
)


def available_ports() -> list[str]:
    return [port.device for port in list_ports.comports()]


class FirmwareUpgradeError(RuntimeError):
    pass


class SerialWorker(QThread):
    telemetry_received = pyqtSignal(object)
    configuration_received = pyqtSignal(object)
    ack_received = pyqtSignal(dict)
    status_changed = pyqtSignal(str)
    error = pyqtSignal(str)
    firmware_progress = pyqtSignal(int, str)
    firmware_finished = pyqtSignal(bool, str)

    def __init__(self, port: str, baudrate: int = 115200) -> None:
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self._running = False
        self._upgrade_active = False
        self._tx_queue: queue.Queue[bytes] = queue.Queue()
        self._firmware_queue: queue.Queue[tuple[str, bytes]] = queue.Queue(maxsize=1)
        self._seq = 1

    def stop(self) -> None:
        self._running = False

    def send_target_state(self, target_state: int) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_set_target_state(self._next_seq(), target_state))

    def send_manual_air(self, enable: bool, air_bits: int) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_set_manual_air(self._next_seq(), enable, air_bits))

    def send_clear_errors(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_clear_errors(self._next_seq()))

    def update_soc_from_ocv(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_update_soc_from_ocv(self._next_seq()))

    def request_configuration(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_get_configuration(self._next_seq()))

    def save_configuration(self, configuration: BmsConfiguration) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_set_configuration(self._next_seq(), configuration))

    def request_software_reset(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_software_reset(self._next_seq()))

    def send_heartbeat(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_heartbeat(self._next_seq()))

    def send_enter_bootloader(self) -> None:
        if not self._upgrade_active:
            self._tx_queue.put(build_enter_bootloader(self._next_seq()))

    def start_firmware_upgrade(self, filename: str, image: bytes) -> bool:
        if self._upgrade_active or not image:
            return False
        self._upgrade_active = True
        try:
            self._firmware_queue.put_nowait((filename, bytes(image)))
        except queue.Full:
            self._upgrade_active = False
            return False
        return True

    def run(self) -> None:
        parser = FrameParser()
        self._running = True
        try:
            with serial.Serial(self.port, self.baudrate, timeout=0.05) as ser:
                self.status_changed.emit(f"Connected: {self.port}")
                ser.write(build_heartbeat(self._next_seq()))
                while self._running:
                    try:
                        firmware_request = self._firmware_queue.get_nowait()
                    except queue.Empty:
                        firmware_request = None

                    if firmware_request is not None:
                        filename, image = firmware_request
                        self._discard_normal_commands()
                        try:
                            self._perform_firmware_upgrade(ser, filename, image)
                        except FirmwareUpgradeError as exc:
                            self.firmware_finished.emit(False, str(exc))
                        except serial.SerialException:
                            self.firmware_finished.emit(False, "固件升级期间串口连接中断。")
                            raise
                        finally:
                            self._upgrade_active = False
                        parser = FrameParser()
                        continue

                    while not self._tx_queue.empty():
                        ser.write(self._tx_queue.get_nowait())
                    chunk = ser.read(256)
                    if chunk:
                        for frame in parser.feed(chunk):
                            self._handle_frame(frame)
                    else:
                        time.sleep(0.005)
        except serial.SerialException as exc:
            self.error.emit(str(exc))
        finally:
            self.status_changed.emit("Disconnected")
            self._running = False
            self._upgrade_active = False

    def _perform_firmware_upgrade(self, ser, filename: str, image: bytes) -> None:
        self._validate_firmware_image(image)
        image_crc32 = zlib.crc32(image) & 0xFFFFFFFF
        self.firmware_progress.emit(0, f"准备升级 {filename}（{len(image)} 字节）")

        ser.reset_input_buffer()
        ser.reset_output_buffer()
        app_seq = self._next_seq()
        ser.write(build_enter_bootloader(app_seq))
        ser.flush()
        self.firmware_progress.emit(1, "已请求 APP 复位进入 Bootloader")

        # The APP ACK is optional because the target may already be in Bootloader.
        app_parser = FrameParser()
        app_ack_deadline = time.monotonic() + 0.6
        while self._running and time.monotonic() < app_ack_deadline:
            for frame in app_parser.feed(ser.read(256)):
                if frame.msg_type != MsgType.ACK:
                    continue
                try:
                    ack = decode_ack(frame.payload)
                except ValueError:
                    continue
                if (
                    ack["command_seq"] == app_seq
                    and ack["command_type"] == MsgType.ENTER_BOOTLOADER
                ):
                    app_ack_deadline = 0.0
                    break

        if not self._running:
            raise FirmwareUpgradeError("固件升级已取消。")
        time.sleep(0.15)
        ser.reset_input_buffer()

        parser = FrameParser()
        sequence = 1
        sequence = self._boot_exchange(
            ser,
            parser,
            MsgType.BOOT_HELLO,
            sequence,
            build_boot_hello(sequence),
            attempts=25,
            response_timeout=0.3,
        )
        self.firmware_progress.emit(3, "Bootloader 握手成功，正在擦除 APP 区域")

        sequence = self._boot_exchange(
            ser,
            parser,
            MsgType.BOOT_BEGIN,
            sequence,
            build_boot_begin(sequence, len(image), image_crc32),
            attempts=3,
            response_timeout=20.0,
        )

        total = len(image)
        for offset in range(0, total, BOOT_DATA_SIZE):
            if not self._running:
                raise FirmwareUpgradeError("固件升级已取消。")
            chunk = image[offset : offset + BOOT_DATA_SIZE]
            sequence = self._boot_exchange(
                ser,
                parser,
                MsgType.BOOT_DATA,
                sequence,
                build_boot_data(sequence, offset, chunk),
                attempts=5,
                response_timeout=2.0,
            )
            written = offset + len(chunk)
            progress = 5 + int(written * 90 / total)
            self.firmware_progress.emit(progress, f"正在写入：{written}/{total} 字节")

        self.firmware_progress.emit(96, "正在校验整包 CRC 和 APP 向量")
        sequence = self._boot_exchange(
            ser,
            parser,
            MsgType.BOOT_END,
            sequence,
            build_boot_end(sequence),
            attempts=15,
            response_timeout=2.0,
        )
        # END 已确认后固件已经有效。RUN 不等待 ACK，避免“设备已跳转但 RUN ACK
        # 丢失”再次造成假失败；Bootloader 还会在超时后自动启动 APP。
        ser.write(build_boot_run(sequence))
        ser.flush()
        self.firmware_progress.emit(100, "固件升级完成，设备正在启动 APP")
        self.firmware_finished.emit(True, f"{filename} 固件升级成功。")
        time.sleep(0.3)
        ser.reset_input_buffer()

    def _boot_exchange(
        self,
        ser,
        parser: FrameParser,
        command: MsgType,
        sequence: int,
        encoded_frame: bytes,
        *,
        attempts: int,
        response_timeout: float,
    ) -> int:
        for _ in range(attempts):
            if not self._running:
                raise FirmwareUpgradeError("固件升级已取消。")
            ser.write(encoded_frame)
            ser.flush()
            response = self._wait_boot_response(
                ser, parser, int(command), sequence, response_timeout
            )
            if response is None:
                continue
            result = response["result"]
            if result == BootResult.OK:
                expected = (sequence + 1) & 0xFFFF
                if response["expected_seq"] != expected:
                    raise FirmwareUpgradeError(
                        f"Bootloader 返回异常序号：期望 {expected}，"
                        f"收到 {response['expected_seq']}。"
                    )
                return expected
            if result == BootResult.BAD_CRC:
                continue
            try:
                reason = BootResult(result).name
            except ValueError:
                reason = f"UNKNOWN_{result}"
            raise FirmwareUpgradeError(
                f"Bootloader 拒绝命令 0x{int(command):02X}：{reason}。"
            )
        raise FirmwareUpgradeError(
            f"等待 Bootloader 命令 0x{int(command):02X} ACK 超时。"
        )

    def _wait_boot_response(
        self,
        ser,
        parser: FrameParser,
        command: int,
        sequence: int,
        timeout: float,
    ) -> dict[str, int] | None:
        deadline = time.monotonic() + timeout
        while self._running and time.monotonic() < deadline:
            waiting = int(getattr(ser, "in_waiting", 0))
            chunk = ser.read(max(1, min(512, waiting)))
            if not chunk:
                continue
            for frame in parser.feed(chunk):
                if frame.msg_type not in (MsgType.ACK, MsgType.NACK):
                    continue
                try:
                    response = decode_boot_response(frame.payload)
                except ValueError:
                    continue
                if (
                    response["command_seq"] == sequence
                    and response["command_type"] == command
                ):
                    return response
        return None

    @staticmethod
    def _validate_firmware_image(image: bytes) -> None:
        if len(image) < 8 or len(image) > BOOT_MAX_IMAGE_SIZE:
            raise FirmwareUpgradeError(
                f"APP 固件长度必须在 8 到 {BOOT_MAX_IMAGE_SIZE} 字节之间。"
            )
        stack_pointer, reset_vector = struct.unpack_from("<II", image)
        stack_valid = (
            0x20000000 < stack_pointer <= 0x20020000
            or 0x10000000 < stack_pointer <= 0x10010000
        ) and stack_pointer % 8 == 0
        reset_address = reset_vector & ~1
        reset_valid = bool(
            reset_vector & 1
            and BOOT_APP_START <= reset_address < BOOT_APP_START + len(image)
        )
        if not stack_valid or not reset_valid:
            raise FirmwareUpgradeError(
                "所选文件不是有效的 APP .bin：栈地址或复位向量不合法。"
            )

    def _discard_normal_commands(self) -> None:
        while True:
            try:
                self._tx_queue.get_nowait()
            except queue.Empty:
                return

    def _handle_frame(self, frame) -> None:
        try:
            if frame.msg_type == MsgType.TELEMETRY_FULL:
                self.telemetry_received.emit(decode_telemetry(frame.payload))
            elif frame.msg_type == MsgType.CONFIGURATION:
                self.configuration_received.emit(decode_configuration(frame.payload))
            elif frame.msg_type == MsgType.ACK:
                self.ack_received.emit(decode_ack(frame.payload))
        except ValueError as exc:
            self.error.emit(str(exc))

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        if self._seq == 0:
            self._seq = 1
        return seq
