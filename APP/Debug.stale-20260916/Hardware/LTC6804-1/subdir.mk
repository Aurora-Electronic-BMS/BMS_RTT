################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Hardware/LTC6804-1/LTC68041.c 

OBJS += \
./Hardware/LTC6804-1/LTC68041.o 

C_DEPS += \
./Hardware/LTC6804-1/LTC68041.d 


# Each subdirectory must supply rules for building sources it contributes
Hardware/LTC6804-1/%.o Hardware/LTC6804-1/%.su Hardware/LTC6804-1/%.cyclo: ../Hardware/LTC6804-1/%.c Hardware/LTC6804-1/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"C:/Users/33011/Documents/GitHub/BMS_V1.0/APP/Hardware/LTC6804-1" -I"C:/Users/33011/Documents/GitHub/BMS_V1.0/APP/Hardware/can-halal" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Hardware-2f-LTC6804-2d-1

clean-Hardware-2f-LTC6804-2d-1:
	-$(RM) ./Hardware/LTC6804-1/LTC68041.cyclo ./Hardware/LTC6804-1/LTC68041.d ./Hardware/LTC6804-1/LTC68041.o ./Hardware/LTC6804-1/LTC68041.su

.PHONY: clean-Hardware-2f-LTC6804-2d-1

