from __future__ import annotations

from datetime import datetime
from pathlib import Path
import time

from PyQt6.QtCore import QTimer, Qt, pyqtSignal
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QAbstractSpinBox,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFrame,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QSizePolicy,
    QSplitter,
    QStyle,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .firmware_config import CellVoltageThresholds, default_cell_voltage_thresholds
from .logger import BmsCsvLogger
from .protocol import (
    AIR_NEG,
    AIR_POS,
    AIR_PRE,
    CELL_CHANNELS,
    CELL_GROUPS,
    BmsConfiguration,
    ControlMode,
    MsgType,
    TEMP_CHANNELS,
    TEMP_GROUPS,
    TS_ERROR_INTERNAL_COMM_ERROR,
    TS_ERROR_NONE,
    TS_ERROR_PRECHARGE_TIMEOUT,
    TS_ERROR_SHUNT_OVERCURRENT,
    TS_ERROR_SHUNT_OVERTEMP,
    TS_ERROR_SHUNT_TIMEOUT,
    TS_ERROR_SLAVE_OVERTEMP,
    TS_ERROR_SLAVE_OVERVOLT,
    TS_ERROR_SLAVE_UNDERVOLT,
    TS_ERROR_VOLTAGE_MISMATCH,
    TargetState,
    Telemetry,
    cell_sequence_number,
    is_valid_cell_slot,
    slot_index,
)
from .serial_worker import SerialWorker, available_ports
from .simulator import TelemetrySimulator


STATE_NAMES = {
    0: "INACTIVE",
    1: "ACTIVE",
    2: "PRECHARGE",
    3: "DISCHARGE",
    4: "ERROR",
    5: "PRE_HOLD",
    6: "CHARGE",
}

ERROR_KIND_NAMES = {
    TS_ERROR_SLAVE_OVERVOLT: "Slave OV",
    TS_ERROR_SLAVE_UNDERVOLT: "Slave UV",
    TS_ERROR_SLAVE_OVERTEMP: "Slave OT",
    TS_ERROR_SHUNT_TIMEOUT: "Shunt Timeout",
    TS_ERROR_SHUNT_OVERCURRENT: "Shunt OC",
    TS_ERROR_SHUNT_OVERTEMP: "Shunt OT",
    TS_ERROR_VOLTAGE_MISMATCH: "Voltage Mismatch",
    TS_ERROR_INTERNAL_COMM_ERROR: "Internal Comm Error",
    TS_ERROR_PRECHARGE_TIMEOUT: "Precharge Timeout",
}

MANUAL_AIR_PASSWORD = "sztu"
NO_COMM_CELL_MV = 65535
CC_FONT_SIZE_PX = 30


CONFIGURATION_FIELDS = (
    ("Pack", "total_ic", "TOTAL_IC", 1, 12),
    ("State machine", "discharge_duration_ms", "Discharge duration (ms)", 0, 300000),
    ("State machine", "no_error_time_ms", "No-error time (ms)", 0, 300000),
    ("State machine", "air_close_wait_ms", "AIR close wait (ms)", 0, 10000),
    ("State machine", "precharge_95_wait_time_ms", "Precharge 95% timeout (ms)", 1, 300000),
    ("State machine", "pre_hold_time_ms", "Pre-hold time (ms)", 0, 60000),
    ("State machine", "max_state_hold_time_ms", "Max state hold (ms)", 0, 60000),
    ("State machine", "drive_mode_timeout_ms", "Drive-mode timeout (ms)", 1, 300000),
    ("State machine", "max_charge_volt_dV", "Max charge voltage (0.1 V)", 0, 10000),
    ("State machine", "max_charge_curr_dA", "Max charge current (0.1 A)", 0, 50000),
    ("Shunt", "shunt_timeout_ms", "Shunt timeout (ms)", 1, 60000),
    ("Shunt", "shunt_overcurrent_discharge_mA", "Discharge overcurrent (mA)", 0, 1000000),
    ("Shunt", "shunt_overcurrent_charge_mA", "Charge overcurrent (mA)", 0, 1000000),
    ("Shunt", "shunt_overcurrent_regen_mA", "Regen overcurrent (mA)", 0, 1000000),
    ("Shunt", "shunt_overtemp_dC", "Shunt overtemperature (0.1 C)", -500, 2000),
    ("Cell", "max_cell_volt_mV", "Max cell voltage (mV)", 2500, 5000),
    ("Cell", "min_cell_volt_mV", "Min cell voltage (mV)", 1500, 4500),
    ("Cell", "max_cell_temp_C", "Max cell temperature (C)", -40, 120),
    ("Cell", "min_cell_temp_C", "Min cell temperature (C)", -40, 100),
    ("Cell", "pec_current_threshold_mA", "PEC current threshold (mA)", 0, 1000000),
    ("ErrorMonitor delays", "shunt_timeout_confirm_ms", "Shunt timeout confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "shunt_overcurrent_confirm_ms", "Shunt overcurrent confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "shunt_overtemp_confirm_ms", "Shunt overtemperature confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "shunt_voltage_mismatch_confirm_ms", "Voltage mismatch confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "slave_overvolt_confirm_ms", "Cell overvoltage confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "slave_undervolt_confirm_ms", "Cell undervoltage confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "slave_overtemp_confirm_ms", "Cell overtemperature confirmation (ms)", 0, 600000),
    ("ErrorMonitor delays", "slave_comm_confirm_ms", "Cell communication confirmation (ms)", 0, 600000),
)

CAN_BAUDRATE_FIELDS = (
    ("can1_baudrate", "CAN1 baud rate"),
    ("can2_baudrate", "CAN2 baud rate"),
)
CAN_BAUDRATE_OPTIONS = (
    ("250 kbit/s", 250000),
    ("500 kbit/s", 500000),
)


class ClickableLabel(QLabel):
    clicked = pyqtSignal()

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()
        super().mouseReleaseEvent(event)


class MetricTile(QFrame):
    def __init__(self, title: str, title_click_handler=None) -> None:
        super().__init__()
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setObjectName("metricTile")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        self.title = ClickableLabel(title) if title_click_handler is not None else QLabel(title)
        self.title.setObjectName("metricTitle")
        if title_click_handler is not None:
            self.title.clicked.connect(title_click_handler)
        self.value = QLabel("--")
        self.value.setObjectName("metricValue")
        self.value.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        layout.addWidget(self.title)
        layout.addWidget(self.value)

    def set_value(self, text: str) -> None:
        self.value.setText(text)


class ConfigurationSpinBox(QSpinBox):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setMinimumHeight(30)
        self.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        self.setStyleSheet(
            """
            QSpinBox {
                background: #FFFFFF;
                border: 1px solid #C7C7CC;
                border-radius: 10px;
                padding: 0 12px;
            }
            QSpinBox:focus {
                border: 2px solid #0A84FF;
                padding: 0 11px;
            }
            """
        )

    def wheelEvent(self, event) -> None:
        # 配置参数只能通过键盘输入调整，避免滚轮误改。
        event.ignore()


class ConfigurationDialog(QDialog):
    def __init__(self, configuration: BmsConfiguration, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("BMS Configuration")
        self.setMinimumWidth(610)
        self._inputs: dict[str, ConfigurationSpinBox] = {}
        self._baudrate_inputs: dict[str, QComboBox] = {}

        layout = QVBoxLayout(self)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        content_layout = QVBoxLayout(content)
        scroll.setWidget(content)
        layout.addWidget(scroll, 1)

        forms: dict[str, QFormLayout] = {}
        for group_name, field_name, label, minimum, maximum in CONFIGURATION_FIELDS:
            form = forms.get(group_name)
            if form is None:
                group_box = QGroupBox(group_name)
                form = QFormLayout(group_box)
                form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
                form.setLabelAlignment(
                    Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
                )
                forms[group_name] = form
                content_layout.addWidget(group_box)
            input_box = ConfigurationSpinBox()
            input_box.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
            input_box.setRange(minimum, maximum)
            input_box.setValue(getattr(configuration, field_name))
            self._inputs[field_name] = input_box
            form.addRow(label, input_box)

        can_group = QGroupBox("CAN")
        can_form = QFormLayout(can_group)
        can_form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        can_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        for field_name, label in CAN_BAUDRATE_FIELDS:
            combo = QComboBox()
            combo.setMinimumHeight(30)
            for option_label, baudrate in CAN_BAUDRATE_OPTIONS:
                combo.addItem(option_label, baudrate)
            option_index = combo.findData(getattr(configuration, field_name))
            combo.setCurrentIndex(max(option_index, 0))
            self._baudrate_inputs[field_name] = combo
            can_form.addRow(label, combo)
        content_layout.insertWidget(0, can_group)

        content_layout.addStretch(1)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def configuration(self) -> BmsConfiguration:
        values = {
            field_name: input_box.value()
            for field_name, input_box in self._inputs.items()
        }
        values.update({
            field_name: int(combo.currentData())
            for field_name, combo in self._baudrate_inputs.items()
        })
        return BmsConfiguration(**values)


class MainWindow(QMainWindow):
    def __init__(self, simulate: bool = False) -> None:
        super().__init__()
        self.setWindowTitle("SZTU-BMS")
        self.worker: SerialWorker | None = None
        self.bms_connected = False
        self.simulator = TelemetrySimulator() if simulate else None
        self.csv_logger: BmsCsvLogger | None = None
        self.last_frame_monotonic = 0.0
        self.frame_times: list[float] = []
        self.enabled_cell_group_count = 0
        self.cell_group_boxes: dict[int, QGroupBox] = {}
        self.cell_group_no_comm_labels: dict[int, QLabel] = {}
        self.cell_labels: dict[tuple[int, int], QLabel] = {}
        self.temp_labels: dict[tuple[int, int], QLabel] = {}
        self.metrics: dict[str, MetricTile] = {}
        self.target_buttons: list[QPushButton] = []
        self.cell_voltage_thresholds: CellVoltageThresholds = default_cell_voltage_thresholds()
        self.latest_configuration: BmsConfiguration | None = None
        self._configuration_dialog_requested = False
        self._can_baudrate_change_pending = False

        self._build_ui()
        self._apply_styles()
        self._set_bms_controls_enabled(False)
        self.refresh_ports()

        self.frame_watchdog = QTimer(self)
        self.frame_watchdog.timeout.connect(self._update_link_age)
        self.frame_watchdog.start(500)

        self.heartbeat_timer = QTimer(self)
        self.heartbeat_timer.timeout.connect(self._send_heartbeat)
        self.heartbeat_timer.start(200)

        if self.simulator is not None:
            self.status_label.setText("Simulation mode")
            self.connect_button.setEnabled(False)
            self.disconnect_button.setEnabled(False)
            self._set_bms_controls_enabled(False)
            self.configuration_button.setEnabled(True)
            self.sim_timer = QTimer(self)
            self.sim_timer.timeout.connect(self._sample_simulator)
            self.sim_timer.start(200)

    def closeEvent(self, event) -> None:
        self._disconnect_serial()
        if self.csv_logger is not None:
            self.csv_logger.close()
        super().closeEvent(event)

    def _build_ui(self) -> None:
        root = QWidget()
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(10, 10, 10, 10)
        root_layout.setSpacing(10)

        root_layout.addWidget(self._build_connection_bar())
        root_layout.addLayout(self._build_metrics())

        splitter = QSplitter()
        splitter.setChildrenCollapsible(False)
        splitter.addWidget(self._build_data_tabs())
        splitter.addWidget(self._scrollable(self._build_side_panel()))
        splitter.setSizes([1120, 330])
        splitter.setStretchFactor(0, 1)
        root_layout.addWidget(splitter, 1)
        self.setCentralWidget(root)

    def _build_connection_bar(self) -> QWidget:
        bar = QFrame()
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(0, 0, 0, 0)
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh_ports)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self._connect_serial)
        self.disconnect_button = QPushButton("Disconnect")
        self.disconnect_button.clicked.connect(self._disconnect_serial)
        self.disconnect_button.setEnabled(False)
        self.firmware_upgrade_button = QPushButton("Firmware Upgrade")
        self.firmware_upgrade_button.clicked.connect(self._start_firmware_upgrade)
        self.firmware_upgrade_button.setEnabled(False)
        self.configuration_button = QPushButton("Configuration")
        self.configuration_button.clicked.connect(self._open_configuration)
        self.configuration_button.setEnabled(False)
        self.clear_errors_button = QPushButton("Clear Faults")
        self.clear_errors_button.clicked.connect(self._clear_errors)
        self.clear_errors_button.setEnabled(False)
        self.log_checkbox = QCheckBox("CSV log")
        self.log_checkbox.stateChanged.connect(self._toggle_logging)
        self.log_path_label = QLabel("Log: off")
        self.log_path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.log_path_label.setMinimumWidth(0)
        self.log_path_label.setSizePolicy(
            QSizePolicy.Policy.Preferred,
            QSizePolicy.Policy.Preferred,
        )
        self.status_label = QLabel("Disconnected")
        self.status_label.setObjectName("statusLabel")
        self.cc_label = QLabel("@cc")
        cc_font = QFont(self.cc_label.font())
        cc_font.setBold(True)
        cc_font.setItalic(True)
        cc_font.setUnderline(True)
        self.cc_label.setFont(cc_font)
        self.cc_label.setStyleSheet(f"font-size: {CC_FONT_SIZE_PX}px;")

        layout.addWidget(QLabel("Port"))
        layout.addWidget(self.port_combo)
        layout.addWidget(self.refresh_button)
        layout.addWidget(self.connect_button)
        layout.addWidget(self.disconnect_button)
        layout.addWidget(self.firmware_upgrade_button)
        layout.addWidget(self.configuration_button)
        layout.addWidget(self.clear_errors_button)
        layout.addSpacing(16)
        layout.addWidget(self.log_checkbox)
        layout.addWidget(self.log_path_label)
        layout.addSpacing(16)
        layout.addWidget(self.status_label)
        layout.addStretch(1)
        layout.addWidget(self.cc_label)
        return bar

    def _build_metrics(self) -> QHBoxLayout:
        layout = QHBoxLayout()
        for key, title in [
            ("pack", "Pack Voltage"),
            ("bus", "Bus Voltage"),
            ("current", "Current"),
            ("soc", "SOC"),
            ("min_cell", "Min Cell"),
            ("max_cell", "Max Cell"),
            ("min_temp", "Min Temp"),
            ("max_temp", "Max Temp"),
            ("state", "State"),
        ]:
            tile = MetricTile(title, self._confirm_soc_update if key == "soc" else None)
            self.metrics[key] = tile
            layout.addWidget(tile)
        return layout

    def _build_data_tabs(self) -> QTabWidget:
        tabs = QTabWidget()
        tabs.addTab(self._scrollable(self._build_cell_grid()), "Cell Voltages")
        tabs.addTab(self._scrollable(self._build_temp_grid()), "Temperatures")
        return tabs

    def _scrollable(self, child: QWidget) -> QScrollArea:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(child)
        return scroll

    def _build_cell_grid(self) -> QWidget:
        container = QWidget()
        layout = QGridLayout(container)
        layout.setSpacing(10)
        for group in range(1, CELL_GROUPS + 1):
            group_box = QGroupBox(f"Group {group:02d}")
            group_box.setObjectName("cellGroupBox")
            self.cell_group_boxes[group] = group_box
            group_layout = QGridLayout(group_box)
            group_layout.setSpacing(5)
            no_comm_label = QLabel("No comm")
            no_comm_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            no_comm_label.setObjectName("cellGroupNoComm")
            no_comm_label.setVisible(False)
            self.cell_group_no_comm_labels[group] = no_comm_label
            group_layout.addWidget(no_comm_label, 0, 0, 1, 3)
            for channel in range(1, CELL_CHANNELS + 1):
                label = QLabel()
                label.setAlignment(Qt.AlignmentFlag.AlignCenter)
                label.setMinimumSize(92, 46)
                label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
                label.setObjectName("cellLabel")
                self.cell_labels[(group, channel)] = label
                group_layout.addWidget(label, (channel - 1) // 3 + 1, (channel - 1) % 3)
                if not is_valid_cell_slot(group, channel):
                    label.setText("N/A")
                    label.setProperty("state", "invalid")
            layout.addWidget(group_box, (group - 1) // 3, (group - 1) % 3)
            self._set_cell_group_state(group, group <= self.enabled_cell_group_count, False)
        return container

    def _build_temp_grid(self) -> QWidget:
        container = QWidget()
        layout = QGridLayout(container)
        layout.setSpacing(10)
        for group in range(1, TEMP_GROUPS + 1):
            group_box = QGroupBox(f"Group {group:02d}")
            group_layout = QGridLayout(group_box)
            group_layout.setSpacing(5)
            for channel in range(1, TEMP_CHANNELS + 1):
                label = QLabel(f"G{group:02d}-T{channel:02d}\n-- C")
                label.setAlignment(Qt.AlignmentFlag.AlignCenter)
                label.setMinimumSize(110, 48)
                label.setObjectName("tempLabel")
                self.temp_labels[(group, channel)] = label
                group_layout.addWidget(label, 0, channel - 1)
            layout.addWidget(group_box, (group - 1) // 2, (group - 1) % 2)
        return container

    def _build_side_panel(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(8, 8, 8, 8)

        status_box = QGroupBox("Status")
        status_layout = QGridLayout(status_box)
        self.ts_state_label = QLabel("--")
        self.target_state_label = QLabel("--")
        self.control_mode_label = QLabel("--")
        self.sdc_label = QLabel("--")
        self.hv_active_label = QLabel("--")
        self.total_ic_label = QLabel("--")
        self.error_kind_label = QLabel("--")
        self.error_kind_label.setWordWrap(True)
        self.error_kind_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.error_kind_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)
        for row, (name, widget) in enumerate([
            ("Current", self.ts_state_label),
            ("Target", self.target_state_label),
            ("Control", self.control_mode_label),
            ("SDC", self.sdc_label),
            ("HV Active", self.hv_active_label),
            ("TOTAL_IC", self.total_ic_label),
        ]):
            status_layout.addWidget(QLabel(name), row, 0)
            status_layout.addWidget(widget, row, 1)
        error_row = 6
        status_layout.addWidget(QLabel("Errors"), error_row, 0, Qt.AlignmentFlag.AlignTop)
        status_layout.addWidget(self.error_kind_label, error_row, 1)
        layout.addWidget(status_box)

        relay_box = QGroupBox("Relays")
        relay_layout = QGridLayout(relay_box)
        self.pos_air_label = QLabel("POS: --")
        self.neg_air_label = QLabel("NEG: --")
        self.pre_air_label = QLabel("PRE: --")
        for label in [self.pos_air_label, self.neg_air_label, self.pre_air_label]:
            label.setObjectName("relayLabel")
        relay_layout.addWidget(self.pos_air_label, 0, 0)
        relay_layout.addWidget(self.neg_air_label, 1, 0)
        relay_layout.addWidget(self.pre_air_label, 2, 0)
        layout.addWidget(relay_box)

        target_box = QGroupBox("Target State")
        target_layout = QVBoxLayout(target_box)
        for label, state in [
            ("Set INACTIVE", TargetState.INACTIVE),
            ("Set ACTIVE", TargetState.ACTIVE),
            ("Set CHARGE", TargetState.CHARGE),
        ]:
            button = QPushButton(label)
            button.clicked.connect(lambda checked=False, state=state: self._send_target_state(state))
            self.target_buttons.append(button)
            target_layout.addWidget(button)
        layout.addWidget(target_box)

        manual_box = QGroupBox("Manual AIR Debug")
        manual_layout = QVBoxLayout(manual_box)
        self.manual_unlock = QCheckBox("Unlock independent relay commands")
        self.manual_unlock.stateChanged.connect(self._manual_unlock_changed)
        self.manual_pos = QCheckBox("POS AIR")
        self.manual_neg = QCheckBox("NEG AIR")
        self.manual_pre = QCheckBox("PRE AIR")
        self.manual_apply = QPushButton("Apply Relay Bits")
        self.manual_apply.clicked.connect(self._apply_manual_air)
        self.manual_disable = QPushButton("Exit Manual Mode")
        self.manual_disable.clicked.connect(self._exit_manual_mode)
        for widget in [self.manual_pos, self.manual_neg, self.manual_pre, self.manual_apply, self.manual_disable]:
            widget.setEnabled(False)
        manual_layout.addWidget(self.manual_unlock)
        manual_layout.addWidget(self.manual_pos)
        manual_layout.addWidget(self.manual_neg)
        manual_layout.addWidget(self.manual_pre)
        manual_layout.addWidget(self.manual_apply)
        manual_layout.addWidget(self.manual_disable)
        layout.addWidget(manual_box)

        self.message_label = QLabel("")
        self.message_label.setWordWrap(True)
        layout.addWidget(self.message_label)
        layout.addStretch(1)
        return panel

    def refresh_ports(self) -> None:
        current = self.port_combo.currentText()
        self.port_combo.clear()
        ports = available_ports()
        self.port_combo.addItems(ports)
        if current in ports:
            self.port_combo.setCurrentText(current)

    def _connect_serial(self) -> None:
        port = self.port_combo.currentText()
        if not port:
            QMessageBox.warning(self, "Serial", "No serial port selected.")
            return
        self.last_frame_monotonic = 0.0
        self.frame_times.clear()
        self.worker = SerialWorker(port)
        self.worker.telemetry_received.connect(self._handle_telemetry)
        self.worker.configuration_received.connect(self._handle_configuration)
        self.worker.ack_received.connect(self._handle_ack)
        self.worker.status_changed.connect(self._handle_status)
        self.worker.error.connect(self._handle_error)
        self.worker.firmware_progress.connect(self._handle_firmware_progress)
        self.worker.firmware_finished.connect(self._handle_firmware_finished)
        self.worker.start()
        self.connect_button.setEnabled(False)
        self.disconnect_button.setEnabled(True)

    def _disconnect_serial(self) -> None:
        if self.worker is not None:
            self.worker.stop()
            self.worker.wait(1500)
            self.worker = None
        self.connect_button.setEnabled(self.simulator is None)
        self.disconnect_button.setEnabled(False)
        self.firmware_upgrade_button.setEnabled(False)
        self.configuration_button.setEnabled(False)
        self._set_bms_controls_enabled(False)

    def _sample_simulator(self) -> None:
        if self.simulator is not None:
            self._handle_telemetry(self.simulator.sample())

    def _handle_telemetry(self, telemetry: Telemetry) -> None:
        now = time.monotonic()
        self._record_telemetry_frame(now)

        self.metrics["pack"].set_value(f"{telemetry.pack_voltage_mV / 1000:.3f} V")
        self.metrics["bus"].set_value(f"{telemetry.shunt_voltage_veh_mV / 1000:.3f} V")
        self.metrics["current"].set_value(f"{telemetry.shunt_current_mA / 1000:.2f} A")
        self.metrics["soc"].set_value(
            "--" if telemetry.soc_percent is None else f"{telemetry.soc_percent:.2f} %"
        )
        self.metrics["min_cell"].set_value(f"{telemetry.min_cell_mV / 1000:.3f} V")
        self.metrics["max_cell"].set_value(f"{telemetry.max_cell_mV / 1000:.3f} V")
        self.metrics["min_temp"].set_value(f"{telemetry.min_temp_C} C")
        self.metrics["max_temp"].set_value(f"{telemetry.max_temp_C} C")
        self.metrics["state"].set_value(STATE_NAMES.get(telemetry.ts_state, str(telemetry.ts_state)))

        self.ts_state_label.setText(STATE_NAMES.get(telemetry.ts_state, str(telemetry.ts_state)))
        self.target_state_label.setText(STATE_NAMES.get(telemetry.target_state, str(telemetry.target_state)))
        self.control_mode_label.setText(ControlMode(telemetry.control_mode).name if telemetry.control_mode in [mode.value for mode in ControlMode] else str(telemetry.control_mode))
        self.sdc_label.setText("Closed" if telemetry.sdc_closed else "Open")
        self.hv_active_label.setText("On" if telemetry.hv_active else "Off")
        self._set_enabled_cell_group_count(telemetry.total_ic)
        self._set_cell_voltage_thresholds(telemetry)
        self._set_error_kind_text(telemetry.error_kind)
        self._set_relay_label(self.pos_air_label, "POS", bool(telemetry.air_bits & AIR_POS))
        self._set_relay_label(self.neg_air_label, "NEG", bool(telemetry.air_bits & AIR_NEG))
        self._set_relay_label(self.pre_air_label, "PRE", bool(telemetry.air_bits & AIR_PRE))

        self._update_cell_grid(telemetry)
        self._update_temp_grid(telemetry)
        if self.csv_logger is not None:
            self.csv_logger.write(telemetry)

    def _record_telemetry_frame(self, now: float) -> None:
        self.last_frame_monotonic = now
        self.frame_times.append(now)
        self.frame_times = [item for item in self.frame_times if now - item < 2.0]
        if (
            self.simulator is None
            and self.worker is not None
            and self.worker.isRunning()
        ):
            self.status_label.setText(f"Connected: {self.worker.port}")

    def _update_cell_grid(self, telemetry: Telemetry) -> None:
        for group in range(1, CELL_GROUPS + 1):
            group_enabled = group <= self.enabled_cell_group_count
            group_no_comm = group_enabled and self._cell_group_has_no_comm(telemetry, group)
            self._set_cell_group_state(group, group_enabled, group_no_comm)
            for channel in range(1, CELL_CHANNELS + 1):
                label = self.cell_labels[(group, channel)]
                index = slot_index(group, channel)
                if not group_enabled:
                    cell_number = cell_sequence_number(group, channel)
                    if cell_number is None:
                        label.setText("N/A")
                    else:
                        label.setText(f"G{group:02d}-C{cell_number:03d}\n--")
                    self._set_label_state(label, "group_disabled")
                    continue
                if not telemetry.cell_valid[index]:
                    label.setText("N/A")
                    self._set_label_state(label, "invalid")
                    continue
                value = telemetry.cell_mV[index]
                cell_number = cell_sequence_number(group, channel)
                label.setText(f"G{group:02d}-C{cell_number:03d}\n{value / 1000:.3f} V")
                self._set_label_state(label, "no_comm" if group_no_comm else self._voltage_state(value))

    def _update_temp_grid(self, telemetry: Telemetry) -> None:
        for group in range(1, TEMP_GROUPS + 1):
            for channel in range(1, TEMP_CHANNELS + 1):
                index = (group - 1) * TEMP_CHANNELS + (channel - 1)
                label = self.temp_labels[(group, channel)]
                if group > self.enabled_cell_group_count:
                    label.setText(f"G{group:02d}-T{channel:02d}\n-- C")
                    self._set_label_state(label, "group_disabled")
                    continue
                value = telemetry.temperatures_C[index]
                label.setText(f"G{group:02d}-T{channel:02d}\n{value} C")
                self._set_label_state(label, self._temp_state(value))

    def _set_enabled_cell_group_count(self, total_ic: int) -> None:
        self.enabled_cell_group_count = max(0, min(int(total_ic), CELL_GROUPS))
        self.total_ic_label.setText(str(self.enabled_cell_group_count))

    def _set_cell_voltage_thresholds(self, telemetry: Telemetry) -> None:
        if telemetry.min_cell_threshold_mV >= telemetry.max_cell_threshold_mV:
            self.cell_voltage_thresholds = default_cell_voltage_thresholds()
            return
        self.cell_voltage_thresholds = CellVoltageThresholds(
            min_mV=telemetry.min_cell_threshold_mV,
            max_mV=telemetry.max_cell_threshold_mV,
        )

    def _voltage_state(self, value_mV: int) -> str:
        if (
            value_mV < self.cell_voltage_thresholds.min_mV
            or value_mV > self.cell_voltage_thresholds.max_mV
        ):
            return "alarm"
        warn_low_mV = max(self.cell_voltage_thresholds.min_mV, 3050)
        warn_high_mV = min(self.cell_voltage_thresholds.max_mV, 4250)
        if value_mV < warn_low_mV or value_mV > warn_high_mV:
            return "warn"
        return "normal"

    def _temp_state(self, value_C: int) -> str:
        if value_C >= 50:
            return "alarm"
        if value_C >= 45:
            return "warn"
        return "normal"

    def _set_label_state(self, label: QLabel, state: str) -> None:
        label.setProperty("state", state)
        label.style().unpolish(label)
        label.style().polish(label)

    def _set_cell_group_state(self, group: int, enabled: bool, no_comm: bool) -> None:
        group_box = self.cell_group_boxes[group]
        no_comm_label = self.cell_group_no_comm_labels[group]
        state = "no_comm" if no_comm else "enabled" if enabled else "disabled"
        group_box.setProperty("cell_group_state", state)
        no_comm_label.setVisible(no_comm)
        for widget in [group_box, no_comm_label]:
            widget.style().unpolish(widget)
            widget.style().polish(widget)

    def _cell_group_has_no_comm(self, telemetry: Telemetry, group: int) -> bool:
        valid_indices = [
            slot_index(group, channel)
            for channel in range(1, CELL_CHANNELS + 1)
            if is_valid_cell_slot(group, channel)
        ]
        return bool(valid_indices) and all(
            telemetry.cell_valid[index] and telemetry.cell_mV[index] == NO_COMM_CELL_MV
            for index in valid_indices
        )

    def _set_relay_label(self, label: QLabel, name: str, closed: bool) -> None:
        label.setText(f"{name}: {'Closed' if closed else 'Open'}")
        label.setProperty("relay_state", "closed" if closed else "open")
        label.style().unpolish(label)
        label.style().polish(label)

    def _set_error_kind_text(self, error_kind: int) -> None:
        active_errors = [
            name
            for bit, name in ERROR_KIND_NAMES.items()
            if error_kind & bit
        ]
        known_mask = TS_ERROR_NONE | sum(ERROR_KIND_NAMES)
        unknown_bits = error_kind & ~known_mask
        if unknown_bits:
            active_errors.append(f"Unknown 0x{unknown_bits:04X}")

        if active_errors:
            text = "\n".join(active_errors)
            tooltip = f"ts_error_kind: 0x{error_kind:04X}\n" + "\n".join(active_errors)
        elif error_kind & TS_ERROR_NONE:
            text = "None"
            tooltip = f"ts_error_kind: 0x{error_kind:04X}"
        else:
            text = "Invalid error status"
            tooltip = f"ts_error_kind: 0x{error_kind:04X} (NONE bit is not set)"
        self.error_kind_label.setText(text)
        self.error_kind_label.setToolTip(tooltip)

    def _send_target_state(self, state: TargetState) -> None:
        if not self.bms_connected or self.worker is None:
            self.message_label.setText("Connect BMS before sending target commands.")
            return
        self.worker.send_target_state(state)
        self.message_label.setText(f"Target command sent: {state.name}")

    def _clear_errors(self) -> None:
        if not self.bms_connected or self.worker is None:
            self.message_label.setText("Connect BMS before clearing faults.")
            return
        self.worker.send_clear_errors()
        self.message_label.setText("Clear-fault command sent.")

    def _confirm_soc_update(self) -> None:
        answer = QMessageBox.question(
            self,
            "Update SOC?",
            "Update SOC?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        if not self.bms_connected or self.worker is None:
            self.message_label.setText("Connect BMS before updating SOC.")
            return
        self.worker.update_soc_from_ocv()
        self.message_label.setText("SOC update command sent.")

    def _open_configuration(self) -> None:
        if self.simulator is not None:
            self._configuration_dialog_requested = True
            self._handle_configuration(self.simulator.configuration)
            return
        if not self.bms_connected or self.worker is None:
            self.message_label.setText("Connect BMS before opening configuration.")
            return
        self._configuration_dialog_requested = True
        self.worker.request_configuration()
        self.message_label.setText("Reading configuration from BMS…")

    def _handle_configuration(self, configuration: BmsConfiguration) -> None:
        self.latest_configuration = configuration
        if not self._configuration_dialog_requested:
            return
        self._configuration_dialog_requested = False
        dialog = ConfigurationDialog(configuration, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            self.message_label.setText("Configuration unchanged.")
            return
        if self.simulator is not None:
            self.simulator.save_configuration(dialog.configuration())
            self.latest_configuration = self.simulator.configuration
            self.message_label.setText("Configuration saved for this simulation session.")
            return
        if self.worker is None or not self.bms_connected:
            self.message_label.setText("BMS disconnected before configuration could be saved.")
            return
        updated_configuration = dialog.configuration()
        self._can_baudrate_change_pending = (
            updated_configuration.can1_baudrate != configuration.can1_baudrate
            or updated_configuration.can2_baudrate != configuration.can2_baudrate
        )
        self.worker.save_configuration(updated_configuration)
        self.message_label.setText("Saving configuration to BMS Flash…")

    def _apply_manual_air(self) -> None:
        if not self._manual_enabled():
            return
        bits = 0
        if self.manual_pos.isChecked():
            bits |= AIR_POS
        if self.manual_neg.isChecked():
            bits |= AIR_NEG
        if self.manual_pre.isChecked():
            bits |= AIR_PRE
        if self.worker is None:
            self.message_label.setText("Connect BMS before sending manual AIR commands.")
            return
        self.worker.send_manual_air(True, bits)
        self.message_label.setText(f"Manual AIR command sent: 0x{bits:02X}")

    def _exit_manual_mode(self) -> None:
        if self.worker is not None and self.bms_connected:
            self.worker.send_manual_air(False, 0)
        self._clear_manual_air_selection()
        self.manual_unlock.setChecked(False)
        self.message_label.setText("Manual AIR mode disabled.")

    def _clear_manual_air_selection(self) -> None:
        for checkbox in (self.manual_pos, self.manual_neg, self.manual_pre):
            checkbox.setChecked(False)

    def _manual_unlock_changed(self) -> None:
        if self.manual_unlock.isChecked() and not self.bms_connected:
            self.manual_unlock.blockSignals(True)
            self.manual_unlock.setChecked(False)
            self.manual_unlock.blockSignals(False)
            self._set_manual_controls_enabled(False)
            self.message_label.setText("Connect BMS before enabling manual AIR debug.")
            return
        if self.manual_unlock.isChecked():
            enabled = self._prompt_manual_air_password()
            if not enabled:
                self.manual_unlock.blockSignals(True)
                self.manual_unlock.setChecked(False)
                self.manual_unlock.blockSignals(False)
        else:
            enabled = False
        self._set_manual_controls_enabled(enabled)
        if enabled:
            self.message_label.setText("Manual AIR mode unlocked.")

    def _set_bms_controls_enabled(self, connected: bool) -> None:
        self.bms_connected = connected
        for button in self.target_buttons:
            button.setEnabled(connected)
        self.clear_errors_button.setEnabled(connected)
        self.configuration_button.setEnabled(connected)
        self.manual_unlock.setEnabled(connected)
        if not connected:
            self.manual_unlock.blockSignals(True)
            self.manual_unlock.setChecked(False)
            self.manual_unlock.blockSignals(False)
            self._set_manual_controls_enabled(False)
            return
        self._set_manual_controls_enabled(self.manual_unlock.isChecked())

    def _set_manual_controls_enabled(self, enabled: bool) -> None:
        enabled = enabled and self.bms_connected
        for widget in [self.manual_pos, self.manual_neg, self.manual_pre, self.manual_apply, self.manual_disable]:
            widget.setEnabled(enabled)

    def _manual_enabled(self, show_warning: bool = True) -> bool:
        enabled = self.bms_connected and self.manual_unlock.isChecked()
        if show_warning and not enabled:
            if not self.bms_connected:
                QMessageBox.warning(self, "Manual AIR", "Connect BMS first.")
            else:
                QMessageBox.warning(self, "Manual AIR", "Unlock manual relay commands first.")
        return enabled

    def _prompt_manual_air_password(self) -> bool:
        dialog = QDialog(self)
        dialog.setWindowTitle("Manual AIR")
        dialog.setMinimumSize(460, 190)

        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)

        warning_row = QHBoxLayout()
        warning_icon = QLabel()
        icon = self.style().standardIcon(QStyle.StandardPixmap.SP_MessageBoxWarning)
        warning_icon.setPixmap(icon.pixmap(32, 32))
        warning_text = QLabel("Ensure disconnecting battery pack!!")
        warning_text.setObjectName("manualAirWarning")
        warning_text.setWordWrap(True)
        warning_row.addWidget(warning_icon, 0, Qt.AlignmentFlag.AlignVCenter)
        warning_row.addWidget(warning_text, 1, Qt.AlignmentFlag.AlignVCenter)
        layout.addLayout(warning_row)

        password_input = QLineEdit()
        password_input.setEchoMode(QLineEdit.EchoMode.Password)
        password_input.setPlaceholderText("Password")
        layout.addWidget(password_input)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        layout.addWidget(buttons)

        def accept_if_password_matches() -> None:
            if password_input.text() == MANUAL_AIR_PASSWORD:
                dialog.accept()
                return
            QMessageBox.critical(dialog, "Manual AIR", "Incorrect password.")
            password_input.clear()
            password_input.setFocus()

        buttons.accepted.connect(accept_if_password_matches)
        buttons.rejected.connect(dialog.reject)
        password_input.returnPressed.connect(accept_if_password_matches)
        password_input.setFocus()

        return dialog.exec() == QDialog.DialogCode.Accepted

    def _send_heartbeat(self) -> None:
        if self.worker is not None:
            self.worker.send_heartbeat()

    def _start_firmware_upgrade(self) -> None:
        if self.worker is None or not self.worker.isRunning():
            QMessageBox.warning(self, "Firmware Upgrade", "Connect the serial port first.")
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Select APP firmware",
            "",
            "Firmware binary (*.bin)",
        )
        if not path:
            return
        try:
            image = Path(path).read_bytes()
        except OSError as exc:
            QMessageBox.critical(self, "Firmware Upgrade", f"Cannot read firmware file:\n{exc}")
            return
        answer = QMessageBox.question(
            self,
            "Firmware Upgrade",
            f"Upgrade with {Path(path).name} ({len(image)} bytes)?\n\n"
            "Do not disconnect power or the serial cable during the upgrade.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        if not self.worker.start_firmware_upgrade(Path(path).name, image):
            QMessageBox.warning(self, "Firmware Upgrade", "A firmware upgrade is already running.")
            return
        self.firmware_upgrade_button.setEnabled(False)
        self.message_label.setText("Firmware upgrade queued: 0%")

    def _handle_firmware_progress(self, progress: int, message: str) -> None:
        self.message_label.setText(f"Firmware upgrade {progress}%: {message}")

    def _handle_firmware_finished(self, success: bool, message: str) -> None:
        self.firmware_upgrade_button.setEnabled(
            self.worker is not None and self.worker.isRunning()
        )
        self.message_label.setText(message)
        if success:
            QMessageBox.information(self, "Firmware Upgrade", message)
        else:
            QMessageBox.critical(self, "Firmware Upgrade", message)

    def _handle_ack(self, ack: dict[str, int]) -> None:
        if ack["command_type"] == MsgType.UPDATE_SOC_FROM_OCV:
            if ack["result"] == 0:
                self.message_label.setText("SOC updated from the minimum cell voltage and saved to BMS Flash.")
            else:
                self.message_label.setText("SOC update was rejected: no valid cell voltage or Flash save failed.")
            return
        if ack["command_type"] == MsgType.SET_CONFIGURATION:
            if ack["result"] == 0:
                if getattr(self, "_can_baudrate_change_pending", False):
                    if self.worker is not None and self.bms_connected:
                        self.worker.request_software_reset()
                    self.message_label.setText(
                        "Configuration saved. Software reset requested to apply the CAN baud-rate change."
                    )
                else:
                    self.message_label.setText("Configuration saved to BMS Flash.")
                self._can_baudrate_change_pending = False
            else:
                reasons = {
                    1: "BMS must be INACTIVE with all AIR relays open.",
                    2: "One or more values are outside the safe range.",
                    3: "Internal Flash write or verification failed.",
                }
                self.message_label.setText(
                    f"Configuration was rejected: {reasons.get(ack['reason'], 'unknown reason')}"
                )
            return
        self.message_label.setText(
            f"ACK cmd=0x{ack['command_type']:02X} result={ack['result']} reason={ack['reason']}"
        )

    def _handle_status(self, status: str) -> None:
        self.status_label.setText(status)
        if status == "Disconnected":
            self.last_frame_monotonic = 0.0
            self.frame_times.clear()
            self.connect_button.setEnabled(self.simulator is None)
            self.disconnect_button.setEnabled(False)
            self.firmware_upgrade_button.setEnabled(False)
            self.configuration_button.setEnabled(False)
            self._set_bms_controls_enabled(False)
        elif status.startswith("Connected:"):
            self.firmware_upgrade_button.setEnabled(True)
            self._set_bms_controls_enabled(True)
            if self.worker is not None:
                self.worker.request_configuration()

    def _handle_error(self, message: str) -> None:
        self.message_label.setText(message)

    def _toggle_logging(self) -> None:
        if self.log_checkbox.isChecked():
            default_dir = Path(__file__).resolve().parents[1] / "logs"
            default_dir.mkdir(parents=True, exist_ok=True)
            default_name = default_dir / f"bms_log_{datetime.now():%Y%m%d_%H%M%S}.csv"
            path, _ = QFileDialog.getSaveFileName(self, "CSV log", str(default_name), "CSV files (*.csv)")
            if not path:
                self.log_checkbox.setChecked(False)
                return
            self.csv_logger = BmsCsvLogger(path)
            self.csv_logger.start()
            self.log_path_label.setText(f"Log: {Path(path).name}")
            self.log_path_label.setToolTip(f"Log file: {path}")
        else:
            if self.csv_logger is not None:
                self.csv_logger.close()
            self.csv_logger = None
            self.log_path_label.setText("Log: off")
            self.log_path_label.setToolTip("")

    def _update_link_age(self) -> None:
        if not self.last_frame_monotonic:
            return
        age = time.monotonic() - self.last_frame_monotonic
        if age > 2.0 and self.simulator is None:
            self.status_label.setText(f"No telemetry for {age:.1f}s")

    def _apply_styles(self) -> None:
        self.setStyleSheet(
            """
            QWidget {
                font-size: 13px;
            }
            QFrame#metricTile {
                border: 1px solid #C9CDD3;
                border-radius: 6px;
                background: #F8FAFC;
            }
            QLabel#metricTitle {
                color: #5D6470;
                font-size: 11px;
            }
            QLabel#metricValue {
                color: #111827;
                font-size: 18px;
                font-weight: 600;
            }
            QLabel#statusLabel {
                font-weight: 600;
            }
            QLabel#cellLabel, QLabel#tempLabel {
                border: 1px solid #BCC4CF;
                border-radius: 4px;
                padding: 4px;
                font-weight: 600;
            }
            QLabel[state="normal"] {
                background: #E7F5EC;
                color: #123524;
            }
            QLabel[state="warn"] {
                background: #FFF4D6;
                color: #5C4200;
            }
            QLabel[state="alarm"] {
                background: #FCE2E2;
                color: #D92D20;
            }
            QLabel[state="invalid"] {
                background: #ECEFF3;
                color: #7A828D;
            }
            QLabel[state="group_disabled"], QLabel[state="no_comm"] {
                background: #E5E7EB;
                color: #667085;
                border-color: #C7CDD6;
            }
            QGroupBox#cellGroupBox[cell_group_state="disabled"],
            QGroupBox#cellGroupBox[cell_group_state="no_comm"] {
                background: #F1F3F5;
                color: #667085;
            }
            QLabel#cellGroupNoComm {
                color: #5D6470;
                font-size: 15px;
                font-weight: 700;
                padding: 2px;
            }
            QLabel#relayLabel {
                font-weight: 600;
            }
            QLabel#relayLabel[relay_state="closed"] {
                color: #D92D20;
            }
            QLabel#relayLabel[relay_state="open"] {
                color: #15803D;
            }
            QLabel#manualAirWarning {
                color: #B42318;
                font-size: 16px;
                font-weight: 700;
            }
            QGroupBox {
                border: 1px solid #D0D5DD;
                border-radius: 6px;
                margin-top: 8px;
                padding-top: 8px;
                font-weight: 600;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 8px;
                padding: 0 4px;
            }
            QPushButton {
                min-height: 28px;
                padding: 0 10px;
                border: 1px solid palette(mid);
                border-radius: 6px;
                background-color: palette(button);
                color: palette(button-text);
            }
            QPushButton:hover {
                border-color: palette(highlight);
            }
            QPushButton:pressed {
                background-color: palette(midlight);
            }
            QPushButton:focus {
                border-color: palette(highlight);
            }
            QPushButton:disabled {
                color: palette(mid);
            }
            """
        )
