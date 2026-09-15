# Windows Packaging

This folder contains the Windows-only packaging workflow for the BMS host app.
Run these commands on Windows, not macOS.

## Setup

From the repository root:

```bat
cd HostComputer
conda env create -f environment.yml
conda activate bms_pc
python -m pip install pyinstaller pillow
```

If the Conda environment already exists:

```bat
cd HostComputer
conda env update -n bms_pc -f environment.yml
conda activate bms_pc
python -m pip install pyinstaller pillow
```

## Build

```bat
cd HostComputer
windows_packaging\build_windows_exe.bat
```

The generated executable is:

```text
HostComputer\dist\SZTU-BMS\SZTU-BMS.exe
```

The script regenerates `windows_packaging\SZTU-BMS.ico` from `LOGO.jpg` with a
solid white background before each build. The same `.ico` is embedded as the
Windows executable icon and copied into the PyInstaller bundle so the Qt window
title-bar icon also uses the SZTU-BMS cover image.
