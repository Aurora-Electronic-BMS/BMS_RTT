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
set(CMAKE_PROJECT_NAME BMS_RTT)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

#APP vector table relocation
target_compile_definitions(stm32cubemx INTERFACE USER_VECT_TAB_ADDRESS VECT_TAB_OFFSET=0x00010000U)

# RT-Thread 的 context_gcc.S 自带一个强符号 HardFault_Handler，会和 it.c 里的重定义。
# 把它的入口改名，让 it.c 的 HardFault_Handler 先做 AIR 断电安全动作，再转交它转储现场。
target_compile_definitions(RT-Thread PRIVATE HardFault_Handler=rt_hw_hard_fault_handler)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
set(BMS_APP_SOURCES
    Core/Src/bms_app.c
    Core/Src/rt_app.c
    Core/Src/bms_io.c
    Core/Src/bms_config.c
    Core/Src/can_custom.c
    Core/Src/shunt_monitor.c
    Core/Src/slave_monitor.c
    Core/Src/soc_estimation.c
    Core/Src/state_machine.c
    Core/Src/state_machine_core.c
    Core/Src/telemetry_serial.c
    Hardware/can-halal/can-halal.c
)

# 板上已装 LTC6804 AFE（CS = SPI1_CS = PD7），默认编真驱动 Hardware/LTC6804-1/LTC68041.c。
# 板上无 AFE 时用 -DBMS_AFE_STUB=ON 退回 Core/Src/LTC68041_stub.c 桩件。
option(BMS_AFE_STUB "Use LTC6804 stub instead of the real driver" OFF)
if(BMS_AFE_STUB)
    list(APPEND BMS_APP_SOURCES Core/Src/LTC68041_stub.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BMS_AFE_STUB=1)
else()
    list(APPEND BMS_APP_SOURCES Hardware/LTC6804-1/LTC68041.c)
endif()

# 台架自测/故障注入开关。量产构建保持 OFF：回环自测不跑、串口 0x20~0x23 一律拒绝、
# 注入层（bms_io_inject / shunt_inject / slaves_inject / charge_inject）整块不编入。
# 要跑 Boot_test/tools 下的 *.py 时用 -DBMS_TEST_HOOKS=ON 重编。
# 注意：CubeIDE 侧不定义该宏，#if BMS_TEST_HOOKS 求值为 0，即等同于量产构建。
option(BMS_TEST_HOOKS "Compile in bench self-test / fault-injection hooks" OFF)
if(BMS_TEST_HOOKS)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BMS_TEST_HOOKS=1)
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BMS_TEST_HOOKS=0)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${BMS_APP_SOURCES})

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Hardware/LTC6804-1
    Hardware/can-halal
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # 单份源码编译期切换：bms_io/telemetry_serial 走 rt_mutex + rt_vsnprintf 分支
    BMS_USE_RTTHREAD=1
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "CMAKE_OBJCOPY is required to package the APP image")
endif()

add_custom_target(firmware_artifacts ALL
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/package_app_image.py
            --elf $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            --objcopy ${CMAKE_OBJCOPY}
            --out-dir $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>
            --name ${CMAKE_PROJECT_NAME}
            --app-start 0x08010000
            --max-image-size 0xB0000
            --metadata-address 0x080FFF00
            --magic 0xB00710AD
    DEPENDS ${CMAKE_PROJECT_NAME}
    COMMENT "Packaging APP binary, Bootloader metadata, and ST-Link HEX"
    VERBATIM
)
