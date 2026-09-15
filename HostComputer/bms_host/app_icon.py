from __future__ import annotations

import sys
from pathlib import Path

from PyQt6.QtGui import QIcon


def load_app_icon() -> QIcon:
    base_dir = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[1]))
    for path in [
        base_dir / "windows_packaging" / "SZTU-BMS.ico",
        base_dir / "assets" / "SZTU-BMS.icns",
        base_dir / "assets" / "SZTU-BMS-icon-preview.png",
        base_dir / "LOGO.jpg",
    ]:
        if not path.is_file():
            continue
        icon = QIcon(str(path))
        if not icon.isNull():
            return icon
    return QIcon()
