#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${HOST_DIR}"

export PYINSTALLER_CONFIG_DIR="${HOST_DIR}/.pyinstaller"

if ! python -c "import PyInstaller" >/dev/null 2>&1; then
  echo "PyInstaller is not installed in the active Python environment." >&2
  echo "Install it with: python -m pip install pyinstaller" >&2
  exit 1
fi

if ! python -c "import PIL" >/dev/null 2>&1; then
  echo "Pillow is not installed in the active Python environment." >&2
  echo "Install it with: python -m pip install pillow" >&2
  exit 1
fi

python "${HOST_DIR}/macos_packaging/generate_macos_icon.py"

python -m PyInstaller \
  --clean \
  --noconfirm \
  --windowed \
  --name "SZTU-BMS" \
  --icon "${HOST_DIR}/assets/SZTU-BMS.icns" \
  --add-data "${HOST_DIR}/assets/SZTU-BMS.icns:assets" \
  --paths "${HOST_DIR}" \
  package_entry.py

echo "Built: ${HOST_DIR}/dist/SZTU-BMS.app"
