cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME APP_test)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/rt_app.c
    # ---- BMS 应用层（自 BMS_V1.0 迁入，按 RT-Thread 线程组织）----
    Core/Src/bms_app.c
    Core/Src/bms_io.c
    Core/Src/can_custom.c
    Core/Src/shunt_monitor.c
    Core/Src/soc_estimation.c
    Core/Src/slave_monitor.c
    Core/Src/state_machine.c
    Core/Src/state_machine_core.c
    Core/Src/telemetry_serial.c
    Core/Src/LTC68041_stub.c
    Hardware/can-halal/can-halal.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Hardware/can-halal
    Hardware/LTC6804-1
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USER_VECT_TAB_ADDRESS
    VECT_TAB_OFFSET=0x10000
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# =====================================================================
# 以下内容不会被 CubeMX 重新生成覆盖（本文件只生成一次）。
# 作用：修复两个因启动文件不正确而无法运行 RT-Thread 的关键问题。
# =====================================================================

# -- 1) 让 RT-Thread 真正启动：自动替换启动文件 ------------------------
# CubeMX 生成的 cmake/stm32cubemx/CMakeLists.txt 引用了根目录的
# startup_stm32f407xx.s（Reset_Handler 结尾是 bl main，不会启动 RT-Thread）。
# 这里把目标源列表里那个错误的启动文件，换成 bl entry 的正确版本。
get_target_property(_app_srcs ${CMAKE_PROJECT_NAME} SOURCES)
set(_app_fixed_srcs "")
foreach(_src IN LISTS _app_srcs)
    get_filename_component(_src_real "${_src}" REALPATH)
    if(_src_real STREQUAL "${CMAKE_SOURCE_DIR}/startup_stm32f407xx.s")
        list(APPEND _app_fixed_srcs "${CMAKE_SOURCE_DIR}/Core/Startup/startup_stm32f407zgtx.s")
    else()
        list(APPEND _app_fixed_srcs "${_src}")
    endif()
endforeach()
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES SOURCES "${_app_fixed_srcs}")

# -- 2) 保护 RT-Thread 的 .rti_fn 段不被"删无用代码"优化掉 ----------------
# rt_app.c 里用 INIT_APP_EXPORT 登记的函数会放进一个叫 .rti_fn 的特殊段，
# 链接器的 --gc-sections 会以为这段"没人引用"而删掉它，导致 INIT_APP_EXPORT
# 的函数永远不执行。
# 做法：读入 CubeMX 生成的链接脚本，在 .rodata 段里插入一行 KEEP(.rti_fn)，
# 生成一个新链接脚本（build 目录里），并让链接器用这个新脚本。
file(READ "${CMAKE_SOURCE_DIR}/STM32F407xx_FLASH.ld" _app_ld)
string(REGEX REPLACE "\\*\\(\\.rodata\\*\\)" "*(.rodata*)\n    KEEP(*(SORT(.rti_fn*)))" _app_ld "${_app_ld}")
file(WRITE "${CMAKE_BINARY_DIR}/RT_link.ld" "${_app_ld}")
string(REPLACE "${CMAKE_SOURCE_DIR}/STM32F407xx_FLASH.ld" "${CMAKE_BINARY_DIR}/RT_link.ld"
       CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# -- 3) 加大 idle 线程栈，消除栈溢出警告 --------------------------------
target_compile_definitions(RT-Thread PRIVATE IDLE_THREAD_STACK_SIZE=512)

# -- 4) 增大堆空间，避免多线程场景下内存不足 ----------------------------
target_compile_definitions(RT-Thread PRIVATE RT_HEAP_SIZE=4096)

# -- 5) 每次构建后自动生成 .bin（IAP 升级需要，之前只能手动 objcopy）----
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin for IAP")
