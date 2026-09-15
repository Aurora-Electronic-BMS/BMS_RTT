################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.c \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.c 

OBJS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.o 

C_DEPS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.d \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.d 


# Each subdirectory must supply rules for building sources it contributes
Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/%.o Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/%.su Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/%.cyclo: ../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/%.c Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -DBMS_USE_RTTHREAD -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/LTC6804-1" -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/can-halal" -I../RT-Thread -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/components/finsh/ -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/include/ -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-src

clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-src:
	-$(RM) ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/clock.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/components.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/idle.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/ipc.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/irq.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/kservice.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/mem.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/object.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/scheduler.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/thread.su ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/timer.su

.PHONY: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-src

