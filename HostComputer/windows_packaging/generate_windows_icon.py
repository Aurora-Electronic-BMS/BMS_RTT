from __future__ import annotations

from pathlib import Path

from PIL import Image


CANVAS_SIZE = 1024
BACKGROUND = (255, 255, 255, 255)
ICON_SIZES = [
    (16, 16),
    (24, 24),
    (32, 32),
    (48, 48),
    (64, 64),
    (128, 128),
    (256, 256),
]


def main() -> int:
    package_dir = Path(__file__).resolve().parent
    host_dir = package_dir.parent
    source = host_dir / "LOGO.jpg"
    target = package_dir / "SZTU-BMS.ico"
    preview = package_dir / "SZTU-BMS-icon-preview.png"

    logo = Image.open(source).convert("RGBA")
    logo.thumbnail((CANVAS_SIZE, CANVAS_SIZE), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS_SIZE, CANVAS_SIZE), BACKGROUND)
    canvas.alpha_composite(
        logo,
        ((CANVAS_SIZE - logo.width) // 2, (CANVAS_SIZE - logo.height) // 2),
    )

    canvas.save(target, format="ICO", sizes=ICON_SIZES)
    canvas.save(preview, format="PNG")
    print(f"Generated: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
