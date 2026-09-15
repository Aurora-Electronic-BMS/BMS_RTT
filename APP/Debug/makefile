################################################################################
# 自动生成的文件。不要编辑！
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include Middlewares/Third_Party/RealThread_RTOS_RT-Thread/src/subdir.mk
-include Middlewares/Third_Party/RealThread_RTOS_RT-Thread/libcpu/arm/cortex-m4/subdir.mk
-include Middlewares/Third_Party/RealThread_RTOS_RT-Thread/components/finsh/subdir.mk
-include Middlewares/Third_Party/RealThread_RTOS_RT-Thread/bsp/subdir.mk
-include Hardware/can-halal/subdir.mk
-include Drivers/STM32F4xx_HAL_Driver/Src/subdir.mk
-include Core/Startup/subdir.mk
-include Core/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := BMS_RTT
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
BMS_RTT.elf \

MAP_FILES += \
BMS_RTT.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
BMS_RTT.list \


# 所有目标
all: main-build

# 主构建目标
main-build: BMS_RTT.elf secondary-outputs

# 工具调用
BMS_RTT.elf BMS_RTT.map: $(OBJS) $(USER_OBJS) D:\dkt_stm32\DKT\BMS_RTT\APP\STM32F407VGTX_FLASH.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-gcc -o "BMS_RTT.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m4 -T"D:\dkt_stm32\DKT\BMS_RTT\APP\STM32F407VGTX_FLASH.ld" --specs=nosys.specs -Wl,-Map="BMS_RTT.map" -Wl,--gc-sections -static --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -Wl,--end-group
	@echo '已结束构建目标： $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo '已结束构建： $@'
	@echo ' '

BMS_RTT.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "BMS_RTT.list"
	@echo '已结束构建： $@'
	@echo ' '

# 其他目标
clean:
	-$(RM) BMS_RTT.elf BMS_RTT.list BMS_RTT.map default.size.stdout
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified

-include ../makefile.targets
