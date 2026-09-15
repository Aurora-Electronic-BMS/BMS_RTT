################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.c 

OBJS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.o 

C_DEPS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.d 


# Each subdirectory must supply rules for building sources it contributes
Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/%.o Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/%.su Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/%.cyclo: ../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/%.c Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -DBMS_USE_RTTHREAD -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/LTC6804-1" -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/can-halal" -I../RT-Thread -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/components/finsh/ -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/include/ -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-bsp

clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-bsp:
	-$(RM) ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/board.su

.PHONY: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-bsp

