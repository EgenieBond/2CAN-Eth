################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (12.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/can_task.c \
../Core/Src/client_handler.c \
../Core/Src/core_task.c \
../Core/Src/debug_uart.c \
../Core/Src/eth_app.c \
../Core/Src/eth_events.c \
../Core/Src/ethernet_task.c \
../Core/Src/fake_client_source.c \
../Core/Src/freertos.c \
../Core/Src/main.c \
../Core/Src/raw_tcp_server.c \
../Core/Src/slcan_parser.c \
../Core/Src/stm32h7xx_hal_msp.c \
../Core/Src/stm32h7xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32h7xx.c 

OBJS += \
./Core/Src/can_task.o \
./Core/Src/client_handler.o \
./Core/Src/core_task.o \
./Core/Src/debug_uart.o \
./Core/Src/eth_app.o \
./Core/Src/eth_events.o \
./Core/Src/ethernet_task.o \
./Core/Src/fake_client_source.o \
./Core/Src/freertos.o \
./Core/Src/main.o \
./Core/Src/raw_tcp_server.o \
./Core/Src/slcan_parser.o \
./Core/Src/stm32h7xx_hal_msp.o \
./Core/Src/stm32h7xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32h7xx.o 

C_DEPS += \
./Core/Src/can_task.d \
./Core/Src/client_handler.d \
./Core/Src/core_task.d \
./Core/Src/debug_uart.d \
./Core/Src/eth_app.d \
./Core/Src/eth_events.d \
./Core/Src/ethernet_task.d \
./Core/Src/fake_client_source.d \
./Core/Src/freertos.d \
./Core/Src/main.d \
./Core/Src/raw_tcp_server.d \
./Core/Src/slcan_parser.d \
./Core/Src/stm32h7xx_hal_msp.d \
./Core/Src/stm32h7xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32h7xx.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_PWR_LDO_SUPPLY -DUSE_HAL_DRIVER -DSTM32H723xx -c -I../Core/Inc -I../Drivers/STM32H7xx_HAL_Driver/Inc -I../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../Drivers/CMSIS/Include -I../LWIP/App -I../LWIP/Target -I../Middlewares/Third_Party/LwIP/src/include -I../Middlewares/Third_Party/LwIP/system -I../Drivers/BSP/Components/lan8742 -I../Middlewares/Third_Party/LwIP/src/include/netif/ppp -I../Middlewares/Third_Party/LwIP/src/include/lwip -I../Middlewares/Third_Party/LwIP/src/include/lwip/apps -I../Middlewares/Third_Party/LwIP/src/include/lwip/priv -I../Middlewares/Third_Party/LwIP/src/include/lwip/prot -I../Middlewares/Third_Party/LwIP/src/include/netif -I../Middlewares/Third_Party/LwIP/src/include/compat/posix -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/arpa -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/net -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/sys -I../Middlewares/Third_Party/LwIP/src/include/compat/stdc -I../Middlewares/Third_Party/LwIP/system/arch -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I/Eth_Module/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7 -I"C:/Users/Egenie/STM32CubeIDE/Repository/STM32Cube_FW_H7_V1.12.0/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1" -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/can_task.cyclo ./Core/Src/can_task.d ./Core/Src/can_task.o ./Core/Src/can_task.su ./Core/Src/client_handler.cyclo ./Core/Src/client_handler.d ./Core/Src/client_handler.o ./Core/Src/client_handler.su ./Core/Src/core_task.cyclo ./Core/Src/core_task.d ./Core/Src/core_task.o ./Core/Src/core_task.su ./Core/Src/debug_uart.cyclo ./Core/Src/debug_uart.d ./Core/Src/debug_uart.o ./Core/Src/debug_uart.su ./Core/Src/eth_app.cyclo ./Core/Src/eth_app.d ./Core/Src/eth_app.o ./Core/Src/eth_app.su ./Core/Src/eth_events.cyclo ./Core/Src/eth_events.d ./Core/Src/eth_events.o ./Core/Src/eth_events.su ./Core/Src/ethernet_task.cyclo ./Core/Src/ethernet_task.d ./Core/Src/ethernet_task.o ./Core/Src/ethernet_task.su ./Core/Src/fake_client_source.cyclo ./Core/Src/fake_client_source.d ./Core/Src/fake_client_source.o ./Core/Src/fake_client_source.su ./Core/Src/freertos.cyclo ./Core/Src/freertos.d ./Core/Src/freertos.o ./Core/Src/freertos.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/raw_tcp_server.cyclo ./Core/Src/raw_tcp_server.d ./Core/Src/raw_tcp_server.o ./Core/Src/raw_tcp_server.su ./Core/Src/slcan_parser.cyclo ./Core/Src/slcan_parser.d ./Core/Src/slcan_parser.o ./Core/Src/slcan_parser.su ./Core/Src/stm32h7xx_hal_msp.cyclo ./Core/Src/stm32h7xx_hal_msp.d ./Core/Src/stm32h7xx_hal_msp.o ./Core/Src/stm32h7xx_hal_msp.su ./Core/Src/stm32h7xx_it.cyclo ./Core/Src/stm32h7xx_it.d ./Core/Src/stm32h7xx_it.o ./Core/Src/stm32h7xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32h7xx.cyclo ./Core/Src/system_stm32h7xx.d ./Core/Src/system_stm32h7xx.o ./Core/Src/system_stm32h7xx.su

.PHONY: clean-Core-2f-Src

