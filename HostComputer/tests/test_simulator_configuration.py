from dataclasses import replace

from PyQt6.QtWidgets import QDialog

from bms_host.main_window import MainWindow
from bms_host.simulator import TelemetrySimulator


class LabelStub:
    def __init__(self) -> None:
        self.text = ""

    def setText(self, text: str) -> None:
        self.text = text


def test_simulator_configuration_can_be_saved() -> None:
    simulator = TelemetrySimulator()
    updated = replace(simulator.configuration, max_cell_volt_mV=4150)

    simulator.save_configuration(updated)

    assert simulator.configuration == updated


def test_configuration_opens_from_simulator_without_serial_worker() -> None:
    window = MainWindow.__new__(MainWindow)
    window.simulator = TelemetrySimulator()
    window._configuration_dialog_requested = False
    received = []
    window._handle_configuration = received.append

    MainWindow._open_configuration(window)

    assert received == [window.simulator.configuration]
    assert window._configuration_dialog_requested


def test_configuration_save_updates_simulator_session(monkeypatch) -> None:
    simulator = TelemetrySimulator()
    updated = replace(simulator.configuration, max_cell_volt_mV=4150)

    class DialogStub:
        def __init__(self, configuration, parent) -> None:
            assert configuration == simulator.configuration

        def exec(self):
            return QDialog.DialogCode.Accepted

        @staticmethod
        def configuration():
            return updated

    monkeypatch.setattr("bms_host.main_window.ConfigurationDialog", DialogStub)
    window = MainWindow.__new__(MainWindow)
    window.simulator = simulator
    window.latest_configuration = None
    window._configuration_dialog_requested = True
    window.message_label = LabelStub()

    MainWindow._handle_configuration(window, simulator.configuration)

    assert simulator.configuration == updated
    assert window.latest_configuration == updated
    assert window.message_label.text == "Configuration saved for this simulation session."
