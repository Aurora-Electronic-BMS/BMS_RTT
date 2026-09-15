from bms_host.main_window import MainWindow


class FakeLabel:
    def __init__(self, text: str) -> None:
        self.text = text

    def setText(self, text: str) -> None:
        self.text = text


class FakeWorker:
    port = "/dev/cu.test"

    @staticmethod
    def isRunning() -> bool:
        return True


def test_telemetry_recovery_clears_timeout_status() -> None:
    window = MainWindow.__new__(MainWindow)
    window.last_frame_monotonic = 10.0
    window.frame_times = []
    window.simulator = None
    window.worker = FakeWorker()
    window.status_label = FakeLabel("No telemetry for 12.5s")

    MainWindow._record_telemetry_frame(window, 20.0)

    assert window.status_label.text == "Connected: /dev/cu.test"
    assert window.last_frame_monotonic == 20.0
    assert window.frame_times == [20.0]
