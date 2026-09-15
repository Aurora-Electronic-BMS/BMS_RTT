from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct
from typing import Iterable

MAGIC = b"\xB5\x4D"
VERSION = 1
MAX_PAYLOAD_LEN = 1024
BOOT_APP_START = 0x08010000
BOOT_MAX_IMAGE_SIZE = 0xEFF00
BOOT_DATA_SIZE = 256

CELL_GROUPS = 12
CELL_CHANNELS = 12
CELL_SLOTS = CELL_GROUPS * CELL_CHANNELS
TEMP_GROUPS = 12
TEMP_CHANNELS = 4
TEMP_SLOTS = TEMP_GROUPS * TEMP_CHANNELS
VALID_CELL_COUNT = 138
DEFAULT_MIN_CELL_VOLT_MV = 1000
DEFAULT_MAX_CELL_VOLT_MV = 4200

VALID_BITS_LEN = (CELL_SLOTS + 7) // 8


class MsgType(IntEnum):
    TELEMETRY_FULL = 0x01
    ACK = 0x02
    NACK = 0x03
    SET_TARGET_STATE = 0x10
    SET_MANUAL_AIR = 0x11
    HEARTBEAT = 0x12
    ENTER_BOOTLOADER = 0x13
    CLEAR_ERRORS = 0x14
    BOOT_HELLO = 0x30
    BOOT_BEGIN = 0x31
    BOOT_DATA = 0x32
    BOOT_END = 0x33
    BOOT_RUN = 0x34


class TargetState(IntEnum):
    INACTIVE = 0
    ACTIVE = 1
    CHARGE = 6


class ControlMode(IntEnum):
    PASSIVE = 0
    TARGET = 1
    MANUAL_AIR = 2


class AckResult(IntEnum):
    OK = 0
    BAD_CRC = 1
    BAD_LENGTH = 2
    UNKNOWN_COMMAND = 3
    REJECTED = 4


class BootResult(IntEnum):
    OK = 0
    BAD_CRC = 1
    BAD_LENGTH = 2
    BAD_COMMAND = 3
    BAD_STATE = 4
    BAD_SEQUENCE = 5
    FLASH_ERROR = 6
    IMAGE_CRC_ERROR = 7
    VECTOR_ERROR = 8


AIR_POS = 0x01
AIR_NEG = 0x02
AIR_PRE = 0x04

TS_ERROR_NONE = 1 << 0
TS_ERROR_SLAVE_OVERVOLT = 1 << 1
TS_ERROR_SLAVE_UNDERVOLT = 1 << 2
TS_ERROR_SLAVE_OVERTEMP = 1 << 3
TS_ERROR_SHUNT_TIMEOUT = 1 << 4
TS_ERROR_SHUNT_OVERCURRENT = 1 << 5
TS_ERROR_SHUNT_OVERTEMP = 1 << 6
TS_ERROR_VOLTAGE_MISMATCH = 1 << 7
TS_ERROR_INTERNAL_COMM_ERROR = 1 << 8
TS_ERROR_PRECHARGE_TIMEOUT = 1 << 9

FRAME_PREFIX_FORMAT = "<BBHH"
FRAME_PREFIX_LEN = struct.calcsize(FRAME_PREFIX_FORMAT)
# 固定区布局与固件 telemetry_serial.c 的 build_telemetry 逐字节对齐。
# 现行版在 busbar_temp 之后插入 1 字节 SOC(%)，固定区长 52 → 总长 454；
# 下面三种旧长度仅用于解码历史帧，发送一律用现行版。
TELEMETRY_LEGACY_FIXED_FORMAT = "<IBBIBBBBiiiiiHHhhHH"
TELEMETRY_TOTAL_IC_FIXED_FORMAT = TELEMETRY_LEGACY_FIXED_FORMAT + "B"
TELEMETRY_NO_SOC_FIXED_FORMAT = TELEMETRY_TOTAL_IC_FIXED_FORMAT + "HH"
TELEMETRY_FIXED_FORMAT = "<IBBIBBBBiiiiiBHHhhHHBHH"
TELEMETRY_LEGACY_FIXED_LEN = struct.calcsize(TELEMETRY_LEGACY_FIXED_FORMAT)
TELEMETRY_TOTAL_IC_FIXED_LEN = struct.calcsize(TELEMETRY_TOTAL_IC_FIXED_FORMAT)
TELEMETRY_NO_SOC_FIXED_LEN = struct.calcsize(TELEMETRY_NO_SOC_FIXED_FORMAT)
TELEMETRY_FIXED_LEN = struct.calcsize(TELEMETRY_FIXED_FORMAT)
ACK_FORMAT = "<HBBB"
BOOT_RESPONSE_FORMAT = "<HBBH"


@dataclass(frozen=True)
class Frame:
    msg_type: int
    seq: int
    payload: bytes


@dataclass(frozen=True)
class Telemetry:
    uptime_ms: int
    ts_state: int
    target_state: int
    error_kind: int
    control_mode: int
    air_bits: int
    sdc_closed: bool
    hv_active: bool
    pack_voltage_mV: int
    shunt_voltage_bat_mV: int
    shunt_voltage_veh_mV: int
    shunt_current_mA: int
    busbar_temp_dC: int
    min_cell_mV: int
    max_cell_mV: int
    min_temp_C: int
    max_temp_C: int
    cell_valid: tuple[bool, ...]
    cell_mV: tuple[int, ...]
    temperatures_C: tuple[int, ...]
    total_ic: int = CELL_GROUPS
    min_cell_threshold_mV: int = DEFAULT_MIN_CELL_VOLT_MV
    max_cell_threshold_mV: int = DEFAULT_MAX_CELL_VOLT_MV
    soc_percent: int = 0

    @property
    def error_flags(self) -> int:
        return self.error_kind


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def is_valid_cell_slot(group: int, channel: int) -> bool:
    """Return whether a 1-based group/channel cell slot is populated."""
    if group < 1 or group > CELL_GROUPS or channel < 1 or channel > CELL_CHANNELS:
        raise ValueError("cell slot out of range")
    return not (group % 2 == 0 and channel == CELL_CHANNELS)


def valid_cell_slots() -> list[tuple[int, int]]:
    return [
        (group, channel)
        for group in range(1, CELL_GROUPS + 1)
        for channel in range(1, CELL_CHANNELS + 1)
        if is_valid_cell_slot(group, channel)
    ]


def cell_sequence_number(group: int, channel: int) -> int | None:
    """Return the 1-based global cell number, or None for an invalid slot."""
    if group < 1 or group > CELL_GROUPS or channel < 1 or channel > CELL_CHANNELS:
        raise ValueError("cell slot out of range")
    if not is_valid_cell_slot(group, channel):
        return None

    sequence = 0
    for current_group in range(1, CELL_GROUPS + 1):
        for current_channel in range(1, CELL_CHANNELS + 1):
            if is_valid_cell_slot(current_group, current_channel):
                sequence += 1
            if current_group == group and current_channel == channel:
                return sequence
    return None


def slot_index(group: int, channel: int) -> int:
    if group < 1 or group > CELL_GROUPS or channel < 1 or channel > CELL_CHANNELS:
        raise ValueError("cell slot out of range")
    return (group - 1) * CELL_CHANNELS + (channel - 1)


def valid_bits_from_slots(valid_slots: Iterable[bool]) -> bytes:
    flags = list(valid_slots)
    if len(flags) != CELL_SLOTS:
        raise ValueError(f"expected {CELL_SLOTS} cell valid flags")
    out = bytearray(VALID_BITS_LEN)
    for index, valid in enumerate(flags):
        if valid:
            out[index // 8] |= 1 << (index % 8)
    return bytes(out)


def valid_bits_to_slots(data: bytes) -> tuple[bool, ...]:
    if len(data) != VALID_BITS_LEN:
        raise ValueError(f"expected {VALID_BITS_LEN} valid-bit bytes")
    return tuple(bool(data[index // 8] & (1 << (index % 8))) for index in range(CELL_SLOTS))


def default_cell_valid_flags() -> tuple[bool, ...]:
    return tuple(
        is_valid_cell_slot(group, channel)
        for group in range(1, CELL_GROUPS + 1)
        for channel in range(1, CELL_CHANNELS + 1)
    )


def infer_total_ic_from_cell_valid(cell_valid: Iterable[bool]) -> int:
    flags = list(cell_valid)
    if len(flags) != CELL_SLOTS:
        raise ValueError(f"expected {CELL_SLOTS} cell valid flags")
    enabled_groups = [
        group
        for group in range(1, CELL_GROUPS + 1)
        if any(flags[slot_index(group, channel)] for channel in range(1, CELL_CHANNELS + 1))
    ]
    return max(enabled_groups, default=0)


def encode_frame(msg_type: int | MsgType, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD_LEN:
        raise ValueError("payload too large")
    prefix = struct.pack(FRAME_PREFIX_FORMAT, VERSION, int(msg_type), seq & 0xFFFF, len(payload))
    crc = crc16_modbus(prefix + payload)
    return MAGIC + prefix + payload + struct.pack("<H", crc)


class FrameParser:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[Frame]:
        self._buffer.extend(data)
        frames: list[Frame] = []

        while True:
            magic_index = self._buffer.find(MAGIC)
            if magic_index < 0:
                if len(self._buffer) > 1:
                    del self._buffer[:-1]
                return frames
            if magic_index:
                del self._buffer[:magic_index]

            min_frame_len = len(MAGIC) + FRAME_PREFIX_LEN + 2
            if len(self._buffer) < min_frame_len:
                return frames

            version, msg_type, seq, payload_len = struct.unpack_from(
                FRAME_PREFIX_FORMAT, self._buffer, len(MAGIC)
            )
            if version != VERSION or payload_len > MAX_PAYLOAD_LEN:
                del self._buffer[0]
                continue

            frame_len = len(MAGIC) + FRAME_PREFIX_LEN + payload_len + 2
            if len(self._buffer) < frame_len:
                return frames

            payload_start = len(MAGIC) + FRAME_PREFIX_LEN
            payload_end = payload_start + payload_len
            expected_crc = struct.unpack_from("<H", self._buffer, payload_end)[0]
            crc_input = bytes(self._buffer[len(MAGIC):payload_end])
            if crc16_modbus(crc_input) != expected_crc:
                del self._buffer[0]
                continue

            frames.append(Frame(msg_type=msg_type, seq=seq, payload=bytes(self._buffer[payload_start:payload_end])))
            del self._buffer[:frame_len]


def encode_telemetry(seq: int, telemetry: Telemetry) -> bytes:
    if len(telemetry.cell_valid) != CELL_SLOTS:
        raise ValueError(f"expected {CELL_SLOTS} cell-valid flags")
    if len(telemetry.cell_mV) != CELL_SLOTS:
        raise ValueError(f"expected {CELL_SLOTS} cell voltage slots")
    if len(telemetry.temperatures_C) != TEMP_SLOTS:
        raise ValueError(f"expected {TEMP_SLOTS} temperatures")

    fixed = struct.pack(
        TELEMETRY_FIXED_FORMAT,
        telemetry.uptime_ms,
        telemetry.ts_state,
        telemetry.target_state,
        telemetry.error_kind,
        telemetry.control_mode,
        telemetry.air_bits,
        int(telemetry.sdc_closed),
        int(telemetry.hv_active),
        telemetry.pack_voltage_mV,
        telemetry.shunt_voltage_bat_mV,
        telemetry.shunt_voltage_veh_mV,
        telemetry.shunt_current_mA,
        telemetry.busbar_temp_dC,
        max(0, min(int(telemetry.soc_percent), 100)),
        telemetry.min_cell_mV,
        telemetry.max_cell_mV,
        telemetry.min_temp_C,
        telemetry.max_temp_C,
        CELL_SLOTS,
        TEMP_SLOTS,
        max(0, min(int(telemetry.total_ic), CELL_GROUPS)),
        telemetry.min_cell_threshold_mV & 0xFFFF,
        telemetry.max_cell_threshold_mV & 0xFFFF,
    )
    payload = (
        fixed
        + valid_bits_from_slots(telemetry.cell_valid)
        + struct.pack(f"<{CELL_SLOTS}H", *telemetry.cell_mV)
        + struct.pack(f"<{TEMP_SLOTS}h", *telemetry.temperatures_C)
    )
    return encode_frame(MsgType.TELEMETRY_FULL, seq, payload)


def decode_telemetry(payload: bytes) -> Telemetry:
    tail_len = VALID_BITS_LEN + CELL_SLOTS * 2 + TEMP_SLOTS * 2
    legacy_len = TELEMETRY_LEGACY_FIXED_LEN + tail_len
    total_ic_len = TELEMETRY_TOTAL_IC_FIXED_LEN + tail_len
    no_soc_len = TELEMETRY_NO_SOC_FIXED_LEN + tail_len
    current_len = TELEMETRY_FIXED_LEN + tail_len
    if len(payload) not in {legacy_len, total_ic_len, no_soc_len, current_len}:
        raise ValueError(f"bad telemetry payload length: {len(payload)}")

    if len(payload) == current_len:
        fixed_format = TELEMETRY_FIXED_FORMAT
        fixed_len = TELEMETRY_FIXED_LEN
    elif len(payload) == no_soc_len:
        fixed_format = TELEMETRY_NO_SOC_FIXED_FORMAT
        fixed_len = TELEMETRY_NO_SOC_FIXED_LEN
    elif len(payload) == total_ic_len:
        fixed_format = TELEMETRY_TOTAL_IC_FIXED_FORMAT
        fixed_len = TELEMETRY_TOTAL_IC_FIXED_LEN
    else:
        fixed_format = TELEMETRY_LEGACY_FIXED_FORMAT
        fixed_len = TELEMETRY_LEGACY_FIXED_LEN
    values = list(struct.unpack_from(fixed_format, payload, 0))
    # 现行版在 busbar_temp 之后多一个 SOC 字节；先摘出来，其余字段顺序与旧版一致。
    soc_percent = values.pop(13) if len(payload) == current_len else 0
    (
        uptime_ms,
        ts_state,
        target_state,
        error_kind,
        control_mode,
        air_bits,
        sdc_closed,
        hv_active,
        pack_voltage_mV,
        shunt_voltage_bat_mV,
        shunt_voltage_veh_mV,
        shunt_current_mA,
        busbar_temp_dC,
        min_cell_mV,
        max_cell_mV,
        min_temp_C,
        max_temp_C,
        cell_slot_count,
        temp_count,
        *optional_config,
    ) = values
    if cell_slot_count != CELL_SLOTS or temp_count != TEMP_SLOTS:
        raise ValueError("unexpected telemetry slot counts")

    offset = fixed_len
    cell_valid = valid_bits_to_slots(payload[offset:offset + VALID_BITS_LEN])
    offset += VALID_BITS_LEN
    cell_mV = struct.unpack_from(f"<{CELL_SLOTS}H", payload, offset)
    offset += CELL_SLOTS * 2
    temperatures_C = struct.unpack_from(f"<{TEMP_SLOTS}h", payload, offset)
    total_ic = optional_config[0] if optional_config else infer_total_ic_from_cell_valid(cell_valid)
    total_ic = max(0, min(int(total_ic), CELL_GROUPS))
    min_cell_threshold_mV = optional_config[1] if len(optional_config) >= 3 else DEFAULT_MIN_CELL_VOLT_MV
    max_cell_threshold_mV = optional_config[2] if len(optional_config) >= 3 else DEFAULT_MAX_CELL_VOLT_MV
    if min_cell_threshold_mV >= max_cell_threshold_mV:
        min_cell_threshold_mV = DEFAULT_MIN_CELL_VOLT_MV
        max_cell_threshold_mV = DEFAULT_MAX_CELL_VOLT_MV

    return Telemetry(
        uptime_ms=uptime_ms,
        ts_state=ts_state,
        target_state=target_state,
        error_kind=error_kind,
        control_mode=control_mode,
        air_bits=air_bits,
        sdc_closed=bool(sdc_closed),
        hv_active=bool(hv_active),
        pack_voltage_mV=pack_voltage_mV,
        shunt_voltage_bat_mV=shunt_voltage_bat_mV,
        shunt_voltage_veh_mV=shunt_voltage_veh_mV,
        shunt_current_mA=shunt_current_mA,
        busbar_temp_dC=busbar_temp_dC,
        min_cell_mV=min_cell_mV,
        max_cell_mV=max_cell_mV,
        min_temp_C=min_temp_C,
        max_temp_C=max_temp_C,
        cell_valid=cell_valid,
        cell_mV=tuple(cell_mV),
        temperatures_C=tuple(temperatures_C),
        total_ic=total_ic,
        min_cell_threshold_mV=min_cell_threshold_mV,
        max_cell_threshold_mV=max_cell_threshold_mV,
        soc_percent=soc_percent,
    )


def build_set_target_state(seq: int, target_state: int | TargetState) -> bytes:
    return encode_frame(MsgType.SET_TARGET_STATE, seq, struct.pack("<B", int(target_state)))


def build_set_manual_air(seq: int, enable: bool, air_bits: int) -> bytes:
    return encode_frame(MsgType.SET_MANUAL_AIR, seq, struct.pack("<BB", int(enable), air_bits & 0x07))


def build_heartbeat(seq: int) -> bytes:
    return encode_frame(MsgType.HEARTBEAT, seq, b"")


def build_enter_bootloader(seq: int) -> bytes:
    return encode_frame(MsgType.ENTER_BOOTLOADER, seq, b"")


def build_clear_errors(seq: int) -> bytes:
    return encode_frame(MsgType.CLEAR_ERRORS, seq, b"")


def build_boot_hello(seq: int) -> bytes:
    return encode_frame(MsgType.BOOT_HELLO, seq, b"")


def build_boot_begin(seq: int, image_length: int, image_crc32: int) -> bytes:
    if image_length < 8 or image_length > BOOT_MAX_IMAGE_SIZE:
        raise ValueError("APP image length is outside the Bootloader range")
    return encode_frame(
        MsgType.BOOT_BEGIN,
        seq,
        struct.pack("<II", image_length, image_crc32 & 0xFFFFFFFF),
    )


def build_boot_data(seq: int, offset: int, data: bytes) -> bytes:
    if not data or len(data) > BOOT_DATA_SIZE:
        raise ValueError(f"Bootloader DATA must contain 1..{BOOT_DATA_SIZE} bytes")
    if offset < 0 or offset + len(data) > BOOT_MAX_IMAGE_SIZE:
        raise ValueError("Bootloader DATA offset is outside the APP image range")
    return encode_frame(MsgType.BOOT_DATA, seq, struct.pack("<I", offset) + data)


def build_boot_end(seq: int) -> bytes:
    return encode_frame(MsgType.BOOT_END, seq, b"")


def build_boot_run(seq: int) -> bytes:
    return encode_frame(MsgType.BOOT_RUN, seq, b"")


def decode_boot_response(payload: bytes) -> dict[str, int]:
    if len(payload) != struct.calcsize(BOOT_RESPONSE_FORMAT):
        raise ValueError("bad Bootloader response payload length")
    command_seq, command_type, result, expected_seq = struct.unpack(
        BOOT_RESPONSE_FORMAT, payload
    )
    return {
        "command_seq": command_seq,
        "command_type": command_type,
        "result": result,
        "expected_seq": expected_seq,
    }


def encode_ack(seq: int, command_seq: int, command_type: int, result: int, reason: int = 0) -> bytes:
    return encode_frame(
        MsgType.ACK,
        seq,
        struct.pack(ACK_FORMAT, command_seq & 0xFFFF, command_type & 0xFF, result & 0xFF, reason & 0xFF),
    )


def decode_ack(payload: bytes) -> dict[str, int]:
    if len(payload) != struct.calcsize(ACK_FORMAT):
        raise ValueError("bad ACK payload length")
    command_seq, command_type, result, reason = struct.unpack(ACK_FORMAT, payload)
    return {
        "command_seq": command_seq,
        "command_type": command_type,
        "result": result,
        "reason": reason,
    }
