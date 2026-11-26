################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (12.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
S_SRCS += \
../1/Core/Startup/startup_stm32h723zgtx.s 

OBJS += \
./1/Core/Startup/startup_stm32h723zgtx.o 

S_DEPS += \
./1/Core/Startup/startup_stm32h723zgtx.d 


# Each subdirectory must supply rules for building sources it contributes
1/Core/Startup/%.o: ../1/Core/Startup/%.s 1/Core/Startup/subdir.mk
	arm-none-eabi-gcc -mcpu=cortex-m7 -g3 -DDEBUG -c -x assembler-with-cpp -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@" "$<"

clean: clean-1-2f-Core-2f-Startup

clean-1-2f-Core-2f-Startup:
	-$(RM) ./1/Core/Startup/startup_stm32h723zgtx.d ./1/Core/Startup/startup_stm32h723zgtx.o

.PHONY: clean-1-2f-Core-2f-Startup

