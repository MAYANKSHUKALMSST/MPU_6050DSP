################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/boot_log.c \
../Core/Src/boot_manager.c \
../Core/Src/freertos.c \
../Core/Src/hw_290.c \
../Core/Src/main.c \
../Core/Src/mpu6050.c \
../Core/Src/network_interface.c \
../Core/Src/ota_flash.c \
../Core/Src/secure_boot.c \
../Core/Src/sha256.c \
../Core/Src/stm32f7xx_hal_msp.c \
../Core/Src/stm32f7xx_hal_timebase_tim.c \
../Core/Src/stm32f7xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32f7xx.c \
../Core/Src/uart_driver.c 

OBJS += \
./Core/Src/boot_log.o \
./Core/Src/boot_manager.o \
./Core/Src/freertos.o \
./Core/Src/hw_290.o \
./Core/Src/main.o \
./Core/Src/mpu6050.o \
./Core/Src/network_interface.o \
./Core/Src/ota_flash.o \
./Core/Src/secure_boot.o \
./Core/Src/sha256.o \
./Core/Src/stm32f7xx_hal_msp.o \
./Core/Src/stm32f7xx_hal_timebase_tim.o \
./Core/Src/stm32f7xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32f7xx.o \
./Core/Src/uart_driver.o 

C_DEPS += \
./Core/Src/boot_log.d \
./Core/Src/boot_manager.d \
./Core/Src/freertos.d \
./Core/Src/hw_290.d \
./Core/Src/main.d \
./Core/Src/mpu6050.d \
./Core/Src/network_interface.d \
./Core/Src/ota_flash.d \
./Core/Src/secure_boot.d \
./Core/Src/sha256.d \
./Core/Src/stm32f7xx_hal_msp.d \
./Core/Src/stm32f7xx_hal_timebase_tim.d \
./Core/Src/stm32f7xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32f7xx.d \
./Core/Src/uart_driver.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DARM_MATH_CM7 -DUSE_HAL_DRIVER -DSTM32F722xx -c -I../Core/Inc -I"C:/Users/mayan/STM32CubeIDE/workspace_1.18.0/MPU_6050DSP/Drivers/CMSIS/DSP/Include" -I../Drivers/STM32F7xx_HAL_Driver/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F7xx/Include -I../Drivers/CMSIS/Include -I"C:/Users/mayan/STM32CubeIDE/workspace_1.18.0/MPU_6050DSP/Drivers/CMSIS/DSP/Include" -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/boot_log.cyclo ./Core/Src/boot_log.d ./Core/Src/boot_log.o ./Core/Src/boot_log.su ./Core/Src/boot_manager.cyclo ./Core/Src/boot_manager.d ./Core/Src/boot_manager.o ./Core/Src/boot_manager.su ./Core/Src/freertos.cyclo ./Core/Src/freertos.d ./Core/Src/freertos.o ./Core/Src/freertos.su ./Core/Src/hw_290.cyclo ./Core/Src/hw_290.d ./Core/Src/hw_290.o ./Core/Src/hw_290.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/mpu6050.cyclo ./Core/Src/mpu6050.d ./Core/Src/mpu6050.o ./Core/Src/mpu6050.su ./Core/Src/network_interface.cyclo ./Core/Src/network_interface.d ./Core/Src/network_interface.o ./Core/Src/network_interface.su ./Core/Src/ota_flash.cyclo ./Core/Src/ota_flash.d ./Core/Src/ota_flash.o ./Core/Src/ota_flash.su ./Core/Src/secure_boot.cyclo ./Core/Src/secure_boot.d ./Core/Src/secure_boot.o ./Core/Src/secure_boot.su ./Core/Src/sha256.cyclo ./Core/Src/sha256.d ./Core/Src/sha256.o ./Core/Src/sha256.su ./Core/Src/stm32f7xx_hal_msp.cyclo ./Core/Src/stm32f7xx_hal_msp.d ./Core/Src/stm32f7xx_hal_msp.o ./Core/Src/stm32f7xx_hal_msp.su ./Core/Src/stm32f7xx_hal_timebase_tim.cyclo ./Core/Src/stm32f7xx_hal_timebase_tim.d ./Core/Src/stm32f7xx_hal_timebase_tim.o ./Core/Src/stm32f7xx_hal_timebase_tim.su ./Core/Src/stm32f7xx_it.cyclo ./Core/Src/stm32f7xx_it.d ./Core/Src/stm32f7xx_it.o ./Core/Src/stm32f7xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32f7xx.cyclo ./Core/Src/system_stm32f7xx.d ./Core/Src/system_stm32f7xx.o ./Core/Src/system_stm32f7xx.su ./Core/Src/uart_driver.cyclo ./Core/Src/uart_driver.d ./Core/Src/uart_driver.o ./Core/Src/uart_driver.su

.PHONY: clean-Core-2f-Src

