from __future__ import annotations

from pathlib import Path

from PIL import Image


CANVAS_SIZE = 1024
BACKGROUND = (255, 255, 255, 255)
ICON_SIZES = [
    (16, 16),
    (32, 32),
    (64, 64),
    (128, 128),
    (256, 256),
    (512, 512),
    (1024, 1024),
]


def main() -> int:
    host_dir = Path(__file__).resolve().parents[1]
    source = host_dir / "LOGO.jpg"
    target = host_dir / "assets" / "SZTU-BMS.icns"
    preview = host_dir / "assets" / "SZTU-BMS-icon-preview.png"

    target.parent.mkdir(parents=True, exist_ok=True)

    logo = Image.open(source).convert("RGBA")
    logo.thumbnail((CANVAS_SIZE, CANVAS_SIZE), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS_SIZE, CANVAS_SIZE), BACKGROUND)
    canvas.alpha_composite(
        logo,
        ((CANVAS_SIZE - logo.width) // 2, (CANVAS_SIZE - logo.height) // 2),
    )

    canvas.save(target, format="ICNS", sizes=ICON_SIZES)
    canvas.save(preview, format="PNG")
    print(f"Generated: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
