@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "HOST_DIR=%%~fI"

cd /d "%HOST_DIR%"

python -c "import PyInstaller" >nul 2>nul
if errorlevel 1 (
    echo PyInstaller is not installed in the active Python environment.
    echo Install it with: python -m pip install pyinstaller
    exit /b 1
)

python -c "import PIL" >nul 2>nul
if errorlevel 1 (
    echo Pillow is not installed in the active Python environment.
    echo Install it with: python -m pip install pillow
    exit /b 1
)

python "%SCRIPT_DIR%generate_windows_icon.py"
if errorlevel 1 exit /b 1

python -m PyInstaller ^
    --clean ^
    --noconfirm ^
    --windowed ^
    --name "SZTU-BMS" ^
    --icon "%SCRIPT_DIR%SZTU-BMS.ico" ^
    --add-data "%SCRIPT_DIR%SZTU-BMS.ico;windows_packaging" ^
    --paths "%HOST_DIR%" ^
    package_entry.py

if errorlevel 1 exit /b 1

echo Built: %HOST_DIR%\dist\SZTU-BMS\SZTU-BMS.exe
