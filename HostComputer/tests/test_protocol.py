from pathlib import Path
import struct

from bms_host.firmware_config import default_cell_voltage_thresholds
from bms_host.logger import BmsCsvLogger
from bms_host.protocol import (
    AIR_NEG,
    AIR_POS,
    AIR_PRE,
    BmsConfiguration,
    CELL_SLOTS,
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
    MsgType,
    FrameParser,
    Telemetry,
    crc16_modbus,
    decode_ack,
    decode_boot_response,
    decode_configuration,
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
    build_get_configuration,
    build_set_manual_air,
    build_set_configuration,
    build_software_reset,
    build_update_soc_from_ocv,
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


def test_update_soc_from_ocv_command_has_no_payload() -> None:
    frame = FrameParser().feed(build_update_soc_from_ocv(27))[0]

    assert frame.msg_type == MsgType.UPDATE_SOC_FROM_OCV
    assert frame.seq == 27
    assert frame.payload == b""


def test_soc_telemetry_round_trip() -> None:
    telemetry = sample_telemetry()
    telemetry = Telemetry(**{**telemetry.__dict__, "soc_percent": 66.67})
    frame = FrameParser().feed(encode_telemetry(10, telemetry))[0]
    decoded = decode_telemetry(frame.payload)

    assert decoded.soc_percent == 66.67


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

    assert decoded.cell_valid[slot_index(2, 12)] is False
    assert decoded.cell_valid[slot_index(1, 12)] is True
    assert decoded.error_kind == telemetry.error_kind
    assert decoded.total_ic == telemetry.total_ic
    assert decoded.min_cell_threshold_mV == telemetry.min_cell_threshold_mV
    assert decoded.max_cell_threshold_mV == telemetry.max_cell_threshold_mV
    assert decoded.cell_mV == telemetry.cell_mV
    assert decoded.temperatures_C == telemetry.temperatures_C


def test_total_ic_only_telemetry_uses_default_thresholds() -> None:
    telemetry = sample_telemetry()
    frame = FrameParser().feed(encode_telemetry(9, telemetry))[0]
    total_ic_only_payload = frame.payload[:47] + frame.payload[51:]
    decoded = decode_telemetry(total_ic_only_payload)

    assert decoded.total_ic == telemetry.total_ic
    assert decoded.min_cell_threshold_mV == default_cell_voltage_thresholds().min_mV
    assert decoded.max_cell_threshold_mV == default_cell_voltage_thresholds().max_mV


def test_legacy_telemetry_infers_total_ic_and_uses_default_thresholds() -> None:
    telemetry = sample_telemetry()
    frame = FrameParser().feed(encode_telemetry(9, telemetry))[0]
    legacy_payload = frame.payload[:46] + frame.payload[51:]
    decoded = decode_telemetry(legacy_payload)

    assert decoded.total_ic == 12
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


def sample_configuration() -> BmsConfiguration:
    return BmsConfiguration(
        total_ic=10,
        discharge_duration_ms=5000,
        no_error_time_ms=1000,
        air_close_wait_ms=10,
        precharge_95_wait_time_ms=5000,
        pre_hold_time_ms=200,
        max_state_hold_time_ms=500,
        drive_mode_timeout_ms=2000,
        max_charge_volt_dV=5800,
        max_charge_curr_dA=30,
        shunt_timeout_ms=500,
        shunt_overcurrent_discharge_mA=200000,
        shunt_overcurrent_charge_mA=15000,
        shunt_overcurrent_regen_mA=25000,
        shunt_overtemp_dC=1000,
        max_cell_volt_mV=4200,
        min_cell_volt_mV=3000,
        max_cell_temp_C=50,
        min_cell_temp_C=0,
        pec_current_threshold_mA=1000,
        shunt_timeout_confirm_ms=500,
        shunt_overcurrent_confirm_ms=500,
        shunt_overtemp_confirm_ms=1000,
        shunt_voltage_mismatch_confirm_ms=1000,
        slave_overvolt_confirm_ms=1000,
        slave_undervolt_confirm_ms=1000,
        slave_overtemp_confirm_ms=1000,
        slave_comm_confirm_ms=1000,
        can1_baudrate=250000,
        can2_baudrate=500000,
    )


def test_configuration_get_and_set_frames_round_trip() -> None:
    configuration = sample_configuration()
    get_frame = FrameParser().feed(build_get_configuration(10))[0]
    set_frame = FrameParser().feed(build_set_configuration(11, configuration))[0]

    assert get_frame.msg_type == MsgType.GET_CONFIGURATION
    assert get_frame.payload == b""
    assert set_frame.msg_type == MsgType.SET_CONFIGURATION
    assert decode_configuration(set_frame.payload) == configuration


def test_software_reset_command_has_no_payload() -> None:
    frame = FrameParser().feed(build_software_reset(12))[0]

    assert frame.msg_type == MsgType.SOFTWARE_RESET
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
