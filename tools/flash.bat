@echo off
chcp 65001 >nul
REM IAP 固件烧录工具
REM 用法:
REM   flash.bat --app APP_test.bin          [仅烧录 APP，需先有 bootloader]
REM   flash.bat --bootloader Bootloader.bin --app APP_test.bin  [同时烧录两者]

python "%~dp0flash.py" %*
