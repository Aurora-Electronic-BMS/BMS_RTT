from __future__ import annotations

import argparse
import sys

from PyQt6.QtWidgets import QApplication

from .app_icon import load_app_icon
from .main_window import MainWindow


def main() -> int:
    parser = argparse.ArgumentParser(description="BMS PyQt host computer")
    parser.add_argument("--simulate", action="store_true", help="run with generated telemetry")
    args = parser.parse_args()

    app = QApplication(sys.argv)
    icon = load_app_icon()
    if not icon.isNull():
        app.setWindowIcon(icon)
    window = MainWindow(simulate=args.simulate)
    if not icon.isNull():
        window.setWindowIcon(icon)
    available = app.primaryScreen().availableGeometry()
    window.resize(
        min(1500, int(available.width() * 0.92)),
        min(940, int(available.height() * 0.90)),
    )
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
