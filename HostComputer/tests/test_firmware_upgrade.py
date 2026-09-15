import struct
import zlib

import pytest

from bms_host.protocol import (
    FrameParser,
    MsgType,
    decode_boot_response,
    encode_ack,
    encode_frame,
)
from bms_host.serial_worker import FirmwareUpgradeError, SerialWorker


class FakeBootloaderSerial:
    def __init__(self) -> None:
        self.rx = bytearray()
        self.image_length = 0
        self.image_crc32 = 0
        self.image = bytearray()
        self.commands: list[int] = []

    def write(self, data: bytes) -> int:
        frame = FrameParser().feed(data)[0]
        self.commands.append(frame.msg_type)

        if frame.msg_type == MsgType.ENTER_BOOTLOADER:
            self.rx.extend(encode_ack(1, frame.seq, MsgType.ENTER_BOOTLOADER, 0))
            return len(data)

        if frame.msg_type == MsgType.BOOT_BEGIN:
            self.image_length, self.image_crc32 = struct.unpack("<II", frame.payload)
            self.image = bytearray(self.image_length)
        elif frame.msg_type == MsgType.BOOT_DATA:
            offset = struct.unpack_from("<I", frame.payload)[0]
            chunk = frame.payload[4:]
            self.image[offset : offset + len(chunk)] = chunk
        elif frame.msg_type == MsgType.BOOT_END:
            assert zlib.crc32(self.image) & 0xFFFFFFFF == self.image_crc32

        expected_seq = (frame.seq + 1) & 0xFFFF
        payload = struct.pack("<HBBH", frame.seq, frame.msg_type, 0, expected_seq)
        response = encode_frame(MsgType.ACK, frame.seq, payload)
        assert decode_boot_response(FrameParser().feed(response)[0].payload)["result"] == 0
        self.rx.extend(response)
        return len(data)

    def read(self, size: int) -> bytes:
        data = bytes(self.rx[:size])
        del self.rx[:size]
        return data

    def flush(self) -> None:
        pass

    def reset_input_buffer(self) -> None:
        self.rx.clear()

    def reset_output_buffer(self) -> None:
        pass


def valid_test_image(length: int = 700) -> bytes:
    image = bytearray((index & 0xFF) for index in range(length))
    struct.pack_into("<II", image, 0, 0x20020000, 0x08010101)
    return bytes(image)


def test_complete_firmware_upgrade_sequence() -> None:
    worker = SerialWorker("fake")
    worker._running = True
    serial_port = FakeBootloaderSerial()
    image = valid_test_image()

    worker._perform_firmware_upgrade(serial_port, "app.bin", image)

    assert bytes(serial_port.image) == image
    assert serial_port.commands == [
        MsgType.ENTER_BOOTLOADER,
        MsgType.BOOT_HELLO,
        MsgType.BOOT_BEGIN,
        MsgType.BOOT_DATA,
        MsgType.BOOT_DATA,
        MsgType.BOOT_DATA,
        MsgType.BOOT_END,
        MsgType.BOOT_RUN,
    ]


def test_firmware_image_vector_is_checked_before_transfer() -> None:
    with pytest.raises(FirmwareUpgradeError, match="栈地址或复位向量"):
        SerialWorker._validate_firmware_image(bytes(64))


def test_bootloader_command_retries_after_lost_ack() -> None:
    worker = SerialWorker("fake")
    worker._running = True
    serial_port = FakeBootloaderSerial()
    original_write = serial_port.write
    writes = 0

    def drop_first_ack(data: bytes) -> int:
        nonlocal writes
        result = original_write(data)
        writes += 1
        if writes == 1:
            serial_port.rx.clear()
        return result

    serial_port.write = drop_first_ack
    next_sequence = worker._boot_exchange(
        serial_port,
        FrameParser(),
        MsgType.BOOT_HELLO,
        1,
        encode_frame(MsgType.BOOT_HELLO, 1),
        attempts=2,
        response_timeout=0.001,
    )

    assert writes == 2
    assert next_sequence == 2


def test_complete_upgrade_retries_lost_end_ack_then_runs_app() -> None:
    worker = SerialWorker("fake")
    worker._running = True
    serial_port = FakeBootloaderSerial()
    original_write = serial_port.write
    dropped = False

    def drop_first_end_ack(data: bytes) -> int:
        nonlocal dropped
        frame = FrameParser().feed(data)[0]
        result = original_write(data)
        if frame.msg_type == MsgType.BOOT_END and not dropped:
            serial_port.rx.clear()
            dropped = True
        return result

    serial_port.write = drop_first_end_ack
    worker._perform_firmware_upgrade(serial_port, "app.bin", valid_test_image())

    assert dropped
    assert serial_port.commands.count(MsgType.BOOT_END) == 2
    assert serial_port.commands[-1] == MsgType.BOOT_RUN
