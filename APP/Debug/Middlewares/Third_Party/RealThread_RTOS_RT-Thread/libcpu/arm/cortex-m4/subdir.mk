################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.c 

S_UPPER_SRCS += \
../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/context_gcc.S 

OBJS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/context_gcc.o \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.o 

S_UPPER_DEPS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/context_gcc.d 

C_DEPS += \
./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.d 


# Each subdirectory must supply rules for building sources it contributes
Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.o: ../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.S Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/subdir.mk
	arm-none-eabi-gcc -mcpu=cortex-m4 -g3 -DDEBUG -c -Wa,-mimplicit-it=thumb -x assembler-with-cpp -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@" "$<"
Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.o Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.su Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.cyclo: ../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/%.c Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -DBMS_USE_RTTHREAD -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/LTC6804-1" -I"D:/dkt_stm32/DKT/BMS_RTT/APP/Hardware/can-halal" -I../RT-Thread -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/components/finsh/ -I../Middlewares/Third_Party/RealThread_RTOS_RT-Thread/include/ -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-libcpu-2f-arm-2f-cortex-2d-m4

clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-libcpu-2f-arm-2f-cortex-2d-m4:
	-$(RM) ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/context_gcc.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/context_gcc.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.cyclo ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.d ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.o ./Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/cpuport.su

.PHONY: clean-Middlewares-2f-Third_Party-2f-RealThread_RTOS_RT-2d-Thread-2f-libcpu-2f-arm-2f-cortex-2d-m4

