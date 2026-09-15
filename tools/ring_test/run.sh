#!/usr/bin/env bash
# 在宿主上编译并运行 SPSC 环形缓冲并发测试。
#   正常模式必须 PASS；负对照(fault)必须被检出，否则说明校验没有作用。
# 若编译器支持 ThreadSanitizer（通常要 Linux/WSL），则额外跑一遍 TSan。
set -e
cd "$(dirname "$0")"                # -> tools/ring_test

GCC=${GCC:-/c/msys64/ucrt64/bin/gcc.exe}
# cc1.exe 依赖同目录 DLL，必须把 bin 放进 PATH，否则 gcc 会静默退出(1)
export PATH="$(dirname "$GCC"):$PATH"

OUT=ring_test.exe
SRC="ring_port.c ring_test.c"

echo "### build (-O2 -pthread)"
"$GCC" -std=c11 -Wall -Wextra -O2 -pthread $SRC -o "$OUT"

echo
echo "### correct mode (expect PASS)"
"./$OUT"

echo
echo "### negative control: publish-early fault (expect DETECTED)"
"./$OUT" fault

echo
# 探测 TSan：能链接才跑
if printf 'int main(void){return 0;}' > _tsan_probe.c \
   && "$GCC" -fsanitize=thread _tsan_probe.c -o _tsan_probe.exe 2>/dev/null; then
    echo "### ThreadSanitizer available -> rebuild & run under TSan"
    "$GCC" -std=c11 -Wall -Wextra -O1 -g -fsanitize=thread -pthread $SRC -o ring_test_tsan.exe
    ./ring_test_tsan.exe
    rm -f _tsan_probe.c _tsan_probe.exe ring_test_tsan.exe
else
    echo "### ThreadSanitizer NOT available on this toolchain"
    echo "### (需要 Linux/WSL：wsl --install 后在此脚本下用 GCC=... 重跑)"
    rm -f _tsan_probe.c _tsan_probe.exe
fi
