################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Hardware/can-halal/can-halal.c 

OBJS += \
./Hardware/can-halal/can-halal.o 

C_DEPS += \
./Hardware/can-halal/can-halal.d 


# Each subdirectory must supply rules for building sources it contributes
Hardware/can-halal/%.o Hardware/can-halal/%.su Hardware/can-halal/%.cyclo: ../Hardware/can-halal/%.c Hardware/can-halal/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"C:/Users/33011/Documents/GitHub/BMS_V1.0/APP/Hardware/LTC6804-1" -I"C:/Users/33011/Documents/GitHub/BMS_V1.0/APP/Hardware/can-halal" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Hardware-2f-can-2d-halal

clean-Hardware-2f-can-2d-halal:
	-$(RM) ./Hardware/can-halal/can-halal.cyclo ./Hardware/can-halal/can-halal.d ./Hardware/can-halal/can-halal.o ./Hardware/can-halal/can-halal.su

.PHONY: clean-Hardware-2f-can-2d-halal

