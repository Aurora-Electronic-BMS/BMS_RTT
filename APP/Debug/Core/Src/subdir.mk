################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/LTC68041_stub.c \
../Core/Src/bms_app.c \
../Core/Src/bms_io.c \
../Core/Src/can.c \
../Core/Src/can_custom.c \
../Core/Src/gpio.c \
../Core/Src/main.c \
../Core/Src/rt_app.c \
../Core/Src/shunt_monitor.c \
../Core/Src/slave_monitor.c \
../Core/Src/soc_estimation.c \
../Core/Src/spi.c \
../Core/Src/state_machine.c \
../Core/Src/state_machine_core.c \
../Core/Src/stm32f4xx_hal_msp.c \
../Core/Src/stm32f4xx_hal_timebase_tim.c \
../Core/Src/stm32f4xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32f4xx.c \
../Core/Src/telemetry_serial.c \
../Core/Src/usart.c 

OBJS += \
./Core/Src/LTC68041_stub.o \
./Core/Src/bms_app.o \
./Core/Src/bms_io.o \
./Core/Src/can.o \
./Core/Src/can_custom.o \
./Core/Src/gpio.o \
./Core/Src/main.o \
./Core/Src/rt_app.o \
./Core/Src/shunt_monitor.o \
./Core/Src/slave_monitor.o \
./Core/Src/soc_estimation.o \
./Core/Src/spi.o \
./Core/Src/state_machine.o \
./Core/Src/state_machine_core.o \
./Core/Src/stm32f4xx_hal_msp.o \
./Core/Src/stm32f4xx_hal_timebase_tim.o \
./Core/Src/stm32f4xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32f4xx.o \
./Core/Src/telemetry_serial.o \
./Core/Src/usart.o 

C_DEPS += \
./Core/Src/LTC68041_stub.d \
./Core/Src/bms_app.d \
./Core/Src/bms_io.d \
./Core/Src/can.d \
./Core/Src/can_custom.d \
./Core/Src/gpio.d \
./Core/Src/main.d \
./Core/Src/rt_app.d \
./Core/Src/shunt_monitor.d \
./Core/Src/slave_monitor.d \
./Core/Src/soc_estimation.d \
./Core/Src/spi.d \
./Core/Src/state_machine.d \
./Core/Src/state_machine_core.d \
./Core/Src/stm32f4xx_hal_msp.d \
./Core/Src/stm32f4xx_hal_timebase_tim.d \
./Core/Src/stm32f4xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32f4xx.d \
./Core/Src/telemetry_serial.d \
./Core/Src/usart.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -DBMS_USE_RTTHREAD -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/LTC6804-1" -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/can-halal" -I../RT-Thread -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/components/finsh/ -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/include/ -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/LTC68041_stub.cyclo ./Core/Src/LTC68041_stub.d ./Core/Src/LTC68041_stub.o ./Core/Src/LTC68041_stub.su ./Core/Src/bms_app.cyclo ./Core/Src/bms_app.d ./Core/Src/bms_app.o ./Core/Src/bms_app.su ./Core/Src/bms_io.cyclo ./Core/Src/bms_io.d ./Core/Src/bms_io.o ./Core/Src/bms_io.su ./Core/Src/can.cyclo ./Core/Src/can.d ./Core/Src/can.o ./Core/Src/can.su ./Core/Src/can_custom.cyclo ./Core/Src/can_custom.d ./Core/Src/can_custom.o ./Core/Src/can_custom.su ./Core/Src/gpio.cyclo ./Core/Src/gpio.d ./Core/Src/gpio.o ./Core/Src/gpio.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/rt_app.cyclo ./Core/Src/rt_app.d ./Core/Src/rt_app.o ./Core/Src/rt_app.su ./Core/Src/shunt_monitor.cyclo ./Core/Src/shunt_monitor.d ./Core/Src/shunt_monitor.o ./Core/Src/shunt_monitor.su ./Core/Src/slave_monitor.cyclo ./Core/Src/slave_monitor.d ./Core/Src/slave_monitor.o ./Core/Src/slave_monitor.su ./Core/Src/soc_estimation.cyclo ./Core/Src/soc_estimation.d ./Core/Src/soc_estimation.o ./Core/Src/soc_estimation.su ./Core/Src/spi.cyclo ./Core/Src/spi.d ./Core/Src/spi.o ./Core/Src/spi.su ./Core/Src/state_machine.cyclo ./Core/Src/state_machine.d ./Core/Src/state_machine.o ./Core/Src/state_machine.su ./Core/Src/state_machine_core.cyclo ./Core/Src/state_machine_core.d ./Core/Src/state_machine_core.o ./Core/Src/state_machine_core.su ./Core/Src/stm32f4xx_hal_msp.cyclo ./Core/Src/stm32f4xx_hal_msp.d ./Core/Src/stm32f4xx_hal_msp.o ./Core/Src/stm32f4xx_hal_msp.su ./Core/Src/stm32f4xx_hal_timebase_tim.cyclo ./Core/Src/stm32f4xx_hal_timebase_tim.d ./Core/Src/stm32f4xx_hal_timebase_tim.o ./Core/Src/stm32f4xx_hal_timebase_tim.su ./Core/Src/stm32f4xx_it.cyclo ./Core/Src/stm32f4xx_it.d ./Core/Src/stm32f4xx_it.o ./Core/Src/stm32f4xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32f4xx.cyclo ./Core/Src/system_stm32f4xx.d ./Core/Src/system_stm32f4xx.o ./Core/Src/system_stm32f4xx.su ./Core/Src/telemetry_serial.cyclo ./Core/Src/telemetry_serial.d ./Core/Src/telemetry_serial.o ./Core/Src/telemetry_serial.su ./Core/Src/usart.cyclo ./Core/Src/usart.d ./Core/Src/usart.o ./Core/Src/usart.su

.PHONY: clean-Core-2f-Src

