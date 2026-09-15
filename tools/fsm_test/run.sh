#!/usr/bin/env bash
# 在宿主(MSYS2 UCRT64 gcc)上编译并运行状态机纯内核单测。
set -e
cd "$(dirname "$0")/../.."          # -> Boot_test

GCC=/c/msys64/ucrt64/bin/gcc.exe
# cc1.exe 依赖同目录 DLL，必须把 bin 放进 PATH，否则 gcc 会静默退出(1)
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT=tools/fsm_test/fsm_test.exe

"$GCC" -std=c11 -Wall -Wextra -O2 \
    -IAPP_test/Core/Inc \
    APP_test/Core/Src/state_machine_core.c \
    tools/fsm_test/test_fsm.c \
    -o "$OUT"

"$OUT"
