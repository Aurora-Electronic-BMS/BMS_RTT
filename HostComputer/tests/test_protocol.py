from pathlib import Path
import struct

from bms_host.firmware_config import default_cell_voltage_thresholds
from bms_host.logger import BmsCsvLogger
from bms_host.protocol import (
    AIR_NEG,
    AIR_POS,
    AIR_PRE,
    CELL_SLOTS,
    TEMP_SLOTS,
    VALID_BITS_LEN,
    TS_ERROR_INTERNAL_COMM_ERROR,
    TS_ERROR_NONE,
    TS_ERROR_SHUNT_OVERCURRENT,
    TS_ERROR_SHUNT_OVERTEMP,
    TS_ERROR_SHUNT_TIMEOUT,
    TS_ERROR_SLAVE_OVERTEMP,
    TS_ERROR_SLAVE_OVERVOLT,
    TS_ERROR_SLAVE_UNDERVOLT,
    TS_ERROR_VOLTAGE_MISMATCH,
    TS_ERROR_PRECHARGE_TIMEOUT,
    TELEMETRY_FIXED_LEN,
    MsgType,
    FrameParser,
    Telemetry,
    crc16_modbus,
    decode_ack,
    decode_boot_response,
    decode_telemetry,
    default_cell_valid_flags,
    encode_ack,
    encode_frame,
    encode_telemetry,
    infer_total_ic_from_cell_valid,
    build_boot_begin,
    build_boot_data,
    build_boot_end,
    build_boot_hello,
    build_clear_errors,
    build_set_manual_air,
    build_enter_bootloader,
    cell_sequence_number,
    is_valid_cell_slot,
    slot_index,
    valid_cell_slots,
)


def sample_telemetry() -> Telemetry:
    valid = default_cell_valid_flags()
    cells = []
    valid_values = []
    for index in range(CELL_SLOTS):
        value = 3600 + index
        if not valid[index]:
            value = 0
        cells.append(value)
        if valid[index]:
            valid_values.append(value)
    temps = tuple(20 + (index % 12) for index in range(48))
    return Telemetry(
        uptime_ms=1234,
        ts_state=1,
        target_state=1,
        error_kind=TS_ERROR_NONE,
        control_mode=0,
        air_bits=AIR_POS | AIR_NEG,
        sdc_closed=True,
        hv_active=True,
        pack_voltage_mV=sum(valid_values),
        shunt_voltage_bat_mV=sum(valid_values),
        shunt_voltage_veh_mV=sum(valid_values) - 100,
        shunt_current_mA=1000,
        busbar_temp_dC=250,
        min_cell_mV=min(valid_values),
        max_cell_mV=max(valid_values),
        min_temp_C=min(temps),
        max_temp_C=max(temps),
        cell_valid=valid,
        cell_mV=tuple(cells),
        temperatures_C=temps,
        total_ic=8,
        min_cell_threshold_mV=3000,
        max_cell_threshold_mV=4200,
        soc_percent=77,
    )


def test_crc16_known_vector() -> None:
    assert crc16_modbus(b"123456789") == 0x4B37


def test_frame_parser_handles_split_and_sticky_frames() -> None:
    frame1 = encode_frame(MsgType.HEARTBEAT, 1, b"")
    frame2 = encode_frame(MsgType.SET_TARGET_STATE, 2, b"\x01")
    parser = FrameParser()

    assert parser.feed(frame1[:3]) == []
    parsed = parser.feed(frame1[3:] + frame2)

    assert len(parsed) == 2
    assert parsed[0].msg_type == MsgType.HEARTBEAT
    assert parsed[1].payload == b"\x01"


def test_frame_parser_drops_bad_crc() -> None:
    frame = bytearray(encode_frame(MsgType.HEARTBEAT, 1, b""))
    frame[-1] ^= 0xFF
    parser = FrameParser()

    assert parser.feed(bytes(frame)) == []
    assert parser.feed(encode_frame(MsgType.HEARTBEAT, 2, b""))[0].seq == 2


def test_even_groups_channel_12_are_invalid() -> None:
    invalid_slots = [(group, 12) for group in range(2, 13, 2)]
    for group, channel in invalid_slots:
        assert not is_valid_cell_slot(group, channel)
        assert cell_sequence_number(group, channel) is None
    assert is_valid_cell_slot(1, 12)
    assert is_valid_cell_slot(3, 12)
    assert cell_sequence_number(1, 1) == 1
    assert cell_sequence_number(1, 12) == 12
    assert cell_sequence_number(2, 11) == 23
    assert cell_sequence_number(3, 1) == 24
    assert cell_sequence_number(12, 11) == 138
    assert len(valid_cell_slots()) == 138


def test_telemetry_round_trip() -> None:
    telemetry = sample_telemetry()
    frame = FrameParser().feed(encode_telemetry(9, telemetry))[0]
    decoded = decode_telemetry(frame.payload)

    assert len(frame.payload) == TELEMETRY_FIXED_LEN + VALID_BITS_LEN + CELL_SLOTS * 2 + TEMP_SLOTS * 2
    assert decoded.cell_valid[slot_index(2, 12)] is False
    assert decoded.cell_valid[slot_index(1, 12)] is True
    assert decoded.error_kind == telemetry.error_kind
    assert decoded.total_ic == telemetry.total_ic
    assert decoded.min_cell_threshold_mV == telemetry.min_cell_threshold_mV
    assert decoded.max_cell_threshold_mV == telemetry.max_cell_threshold_mV
    assert decoded.soc_percent == telemetry.soc_percent
    assert decoded.cell_mV == telemetry.cell_mV
    assert decoded.temperatures_C == telemetry.temperatures_C


def test_total_ic_only_telemetry_uses_default_thresholds() -> None:
    telemetry = sample_telemetry()
    frame = FrameParser().feed(encode_telemetry(9, telemetry))[0]
    # 现行固定区 52B：去掉 SOC(偏移 34) 与两个阈值(48..51) 得到旧 47B 变体
    total_ic_only_payload = frame.payload[:34] + frame.payload[35:48] + frame.payload[52:]
    decoded = decode_telemetry(total_ic_only_payload)

    assert decoded.total_ic == telemetry.total_ic
    assert decoded.soc_percent == 0
    assert decoded.min_cell_threshold_mV == default_cell_voltage_thresholds().min_mV
    assert decoded.max_cell_threshold_mV == default_cell_voltage_thresholds().max_mV


def test_legacy_telemetry_infers_total_ic_and_uses_default_thresholds() -> None:
    telemetry = sample_telemetry()
    frame = FrameParser().feed(encode_telemetry(9, telemetry))[0]
    # 再去掉 total_ic(偏移 47) 得到旧 46B 变体
    legacy_payload = frame.payload[:34] + frame.payload[35:47] + frame.payload[52:]
    decoded = decode_telemetry(legacy_payload)

    assert decoded.total_ic == 12
    assert decoded.soc_percent == 0
    assert decoded.min_cell_threshold_mV == default_cell_voltage_thresholds().min_mV
    assert decoded.max_cell_threshold_mV == default_cell_voltage_thresholds().max_mV


def test_total_ic_can_be_inferred_from_valid_cell_groups() -> None:
    valid = list(default_cell_valid_flags())
    for group in range(4, 13):
        for channel in range(1, 13):
            valid[slot_index(group, channel)] = False

    assert infer_total_ic_from_cell_valid(valid) == 3


def test_error_kind_masks_match_firmware_layout() -> None:
    fault_mask = (
        TS_ERROR_SLAVE_OVERVOLT
        | TS_ERROR_SLAVE_UNDERVOLT
        | TS_ERROR_SLAVE_OVERTEMP
        | TS_ERROR_SHUNT_TIMEOUT
        | TS_ERROR_SHUNT_OVERCURRENT
        | TS_ERROR_SHUNT_OVERTEMP
        | TS_ERROR_VOLTAGE_MISMATCH
        | TS_ERROR_INTERNAL_COMM_ERROR
        | TS_ERROR_PRECHARGE_TIMEOUT
    )

    assert TS_ERROR_NONE == 0x01
    assert TS_ERROR_SLAVE_OVERVOLT == 0x02
    assert TS_ERROR_SLAVE_UNDERVOLT == 0x04
    assert TS_ERROR_SLAVE_OVERTEMP == 0x08
    assert TS_ERROR_SHUNT_TIMEOUT == 0x10
    assert TS_ERROR_SHUNT_OVERCURRENT == 0x20
    assert TS_ERROR_SHUNT_OVERTEMP == 0x40
    assert TS_ERROR_VOLTAGE_MISMATCH == 0x80
    assert TS_ERROR_INTERNAL_COMM_ERROR == 0x100
    assert TS_ERROR_PRECHARGE_TIMEOUT == 0x200
    assert TS_ERROR_NONE & fault_mask == 0


def test_ack_round_trip() -> None:
    frame = FrameParser().feed(encode_ack(5, 4, MsgType.SET_MANUAL_AIR, 0, 0))[0]
    ack = decode_ack(frame.payload)

    assert ack["command_seq"] == 4
    assert ack["command_type"] == MsgType.SET_MANUAL_AIR
    assert ack["result"] == 0


def test_manual_air_command_allows_independent_bits() -> None:
    frame = FrameParser().feed(build_set_manual_air(7, True, AIR_POS | AIR_PRE))[0]

    assert frame.msg_type == MsgType.SET_MANUAL_AIR
    assert frame.payload == bytes([1, AIR_POS | AIR_PRE])


def test_enter_bootloader_command_has_empty_payload() -> None:
    frame = FrameParser().feed(build_enter_bootloader(8))[0]

    assert frame.msg_type == MsgType.ENTER_BOOTLOADER
    assert frame.seq == 8
    assert frame.payload == b""


def test_clear_errors_command_has_empty_payload() -> None:
    frame = FrameParser().feed(build_clear_errors(9))[0]

    assert frame.msg_type == MsgType.CLEAR_ERRORS
    assert frame.seq == 9
    assert frame.payload == b""


def test_bootloader_upgrade_frames() -> None:
    hello = FrameParser().feed(build_boot_hello(1))[0]
    begin = FrameParser().feed(build_boot_begin(2, 1024, 0x12345678))[0]
    data = FrameParser().feed(build_boot_data(3, 256, b"abc"))[0]
    end = FrameParser().feed(build_boot_end(4))[0]

    assert hello.msg_type == MsgType.BOOT_HELLO and hello.payload == b""
    assert struct.unpack("<II", begin.payload) == (1024, 0x12345678)
    assert struct.unpack("<I", data.payload[:4])[0] == 256
    assert data.payload[4:] == b"abc"
    assert end.msg_type == MsgType.BOOT_END and end.payload == b""


def test_bootloader_response_decode() -> None:
    response = decode_boot_response(struct.pack("<HBBH", 7, MsgType.BOOT_DATA, 0, 8))

    assert response == {
        "command_seq": 7,
        "command_type": MsgType.BOOT_DATA,
        "result": 0,
        "expected_seq": 8,
    }


def test_csv_logger_writes_valid_cells_only(tmp_path: Path) -> None:
    path = tmp_path / "bms.csv"
    logger = BmsCsvLogger(path)
    logger.start()
    logger.write(sample_telemetry())
    logger.close()

    header = path.read_text().splitlines()[0].split(",")
    assert "cell_G02_C12_mV" not in header
    assert "cell_G02_C023_mV" in header
    assert "cell_G03_C024_mV" in header
    assert "cell_G12_C138_mV" in header
    assert "error_kind" in header
    assert "error_flags" not in header
    assert sum(1 for name in header if name.startswith("cell_")) == 138


def test_default_cell_voltage_thresholds_are_header_independent() -> None:
    thresholds = default_cell_voltage_thresholds()

    assert thresholds.min_mV == 1000
    assert thresholds.max_mV == 4200
