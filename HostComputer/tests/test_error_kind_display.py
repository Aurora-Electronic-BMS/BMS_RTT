from bms_host.main_window import MainWindow, STATE_NAMES
from bms_host.protocol import (
    TS_ERROR_INTERNAL_COMM_ERROR,
    TS_ERROR_NONE,
    TS_ERROR_PRECHARGE_TIMEOUT,
    TS_ERROR_SHUNT_TIMEOUT,
    TS_ERROR_SLAVE_OVERVOLT,
)


class LabelStub:
    def __init__(self) -> None:
        self.text = ""
        self.tooltip = ""

    def setText(self, text: str) -> None:
        self.text = text

    def setToolTip(self, tooltip: str) -> None:
        self.tooltip = tooltip


class WindowStub:
    def __init__(self) -> None:
        self.error_kind_label = LabelStub()


def test_error_kind_display_uses_new_bit_layout() -> None:
    window = WindowStub()

    MainWindow._set_error_kind_text(window, TS_ERROR_NONE)
    assert window.error_kind_label.text == "None"

    MainWindow._set_error_kind_text(
        window,
        TS_ERROR_SLAVE_OVERVOLT | TS_ERROR_SHUNT_TIMEOUT,
    )
    assert window.error_kind_label.text == "Slave OV\nShunt Timeout"

    MainWindow._set_error_kind_text(window, TS_ERROR_PRECHARGE_TIMEOUT)
    assert window.error_kind_label.text == "Precharge Timeout"

    MainWindow._set_error_kind_text(window, TS_ERROR_INTERNAL_COMM_ERROR)
    assert window.error_kind_label.text == "Internal Comm Error"


def test_zero_error_kind_is_not_treated_as_no_fault() -> None:
    window = WindowStub()

    MainWindow._set_error_kind_text(window, 0)

    assert window.error_kind_label.text == "Invalid error status"


def test_state_names_match_firmware_pre_hold_layout() -> None:
    assert STATE_NAMES[5] == "PRE_HOLD"
    assert STATE_NAMES[6] == "CHARGE"
    assert "CHARGE_CHECK" not in STATE_NAMES.values()
