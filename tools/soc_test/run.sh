#!/usr/bin/env bash
# 在宿主上编译并运行 SOC 估算单测：直接编译**真实的 soc_estimation.c**，
# 用 stub/ 下的桩头文件隔开固件依赖（shunt_data / min_voltage / HAL_GetTick）。
#
# 同一套断言跑两份源码：
#   rt = Boot_test/APP_test（RT-Thread 版，已接入）
#   bm = BMS_V1.0/APP（裸机版，同步修正、未接入）
# 两份都 PASS，即"两版算法一致"的可执行证据。
set -u
cd "$(dirname "$0")"                # -> tools/soc_test

GCC=${GCC:-/c/msys64/ucrt64/bin/gcc.exe}
# cc1.exe 依赖同目录 DLL，必须把 bin 放进 PATH，否则 gcc 会静默退出(1)
export PATH="$(dirname "$GCC"):$PATH"

CFLAGS="-std=c11 -Wall -Wextra -O2"
COMMON="soc_stubs.c soc_host_test.c"
status=0

# run_one <名字> <soc_estimation.c 所在目录> <其 Core/Inc 目录>
run_one() {
    local name="$1" srcdir="$2" incdir="$3"
    local out="soc_test_${name}.exe"
    echo
    echo "### build [$name] $srcdir/soc_estimation.c"
    if ! "$GCC" $CFLAGS -Istub -I"$incdir" "$srcdir/soc_estimation.c" $COMMON -o "$out"; then
        echo "### BUILD FAILED [$name]"
        status=1
        return
    fi
    echo "### run   [$name]"
    "./$out" || status=1
}

rm -f soc_test_rt.exe soc_test_bm.exe
run_one rt ../../APP_test/Core/Src           ../../APP_test/Core/Inc
run_one bm ../../../BMS_V1.0/APP/Core/Src    ../../../BMS_V1.0/APP/Core/Inc

echo
if [ "$status" -eq 0 ]; then
    echo "### ALL PASS"
else
    echo "### FAILURES PRESENT"
fi
exit "$status"
