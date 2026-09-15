from bms_host.main_window import MainWindow


class CheckBoxStub:
    def __init__(self, checked: bool) -> None:
        self.checked = checked

    def setChecked(self, checked: bool) -> None:
        self.checked = checked


class LabelStub:
    def __init__(self) -> None:
        self.text = ""

    def setText(self, text: str) -> None:
        self.text = text


class WorkerStub:
    def __init__(self) -> None:
        self.commands: list[tuple[bool, int]] = []

    def send_manual_air(self, enable: bool, bits: int) -> None:
        self.commands.append((enable, bits))


class ClearFaultWorkerStub:
    def __init__(self) -> None:
        self.clear_requests = 0

    def send_clear_errors(self) -> None:
        self.clear_requests += 1


def test_exit_manual_mode_releases_and_unchecks_all_relays() -> None:
    window = MainWindow.__new__(MainWindow)
    window.worker = WorkerStub()
    window.bms_connected = True
    window.manual_unlock = CheckBoxStub(True)
    window.manual_pos = CheckBoxStub(True)
    window.manual_neg = CheckBoxStub(True)
    window.manual_pre = CheckBoxStub(True)
    window.message_label = LabelStub()

    MainWindow._exit_manual_mode(window)

    assert window.worker.commands == [(False, 0)]
    assert not window.manual_unlock.checked
    assert not window.manual_pos.checked
    assert not window.manual_neg.checked
    assert not window.manual_pre.checked


def test_clear_errors_sends_one_shot_command_when_connected() -> None:
    window = MainWindow.__new__(MainWindow)
    window.worker = ClearFaultWorkerStub()
    window.bms_connected = True
    window.message_label = LabelStub()

    MainWindow._clear_errors(window)

    assert window.worker.clear_requests == 1
    assert window.message_label.text == "Clear-fault command sent."
