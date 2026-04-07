################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/ota/flash_manager.c \
../Core/Src/ota/ota_agent.c \
../Core/Src/ota/ota_metadata.c 

OBJS += \
./Core/Src/ota/flash_manager.o \
./Core/Src/ota/ota_agent.o \
./Core/Src/ota/ota_metadata.o 

C_DEPS += \
./Core/Src/ota/flash_manager.d \
./Core/Src/ota/ota_agent.d \
./Core/Src/ota/ota_metadata.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/ota/%.o Core/Src/ota/%.su Core/Src/ota/%.cyclo: ../Core/Src/ota/%.c Core/Src/ota/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DARM_MATH_CM7 -DUSE_HAL_DRIVER -DSTM32F722xx -c -I../Core/Inc -I"C:/Users/mayan/STM32CubeIDE/workspace_1.18.0/MPU_6050DSP/Drivers/CMSIS/DSP/Include" -I../Drivers/STM32F7xx_HAL_Driver/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F7xx/Include -I../Drivers/CMSIS/Include -I"C:/Users/mayan/STM32CubeIDE/workspace_1.18.0/MPU_6050DSP/Drivers/CMSIS/DSP/Include" -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src-2f-ota

clean-Core-2f-Src-2f-ota:
	-$(RM) ./Core/Src/ota/flash_manager.cyclo ./Core/Src/ota/flash_manager.d ./Core/Src/ota/flash_manager.o ./Core/Src/ota/flash_manager.su ./Core/Src/ota/ota_agent.cyclo ./Core/Src/ota/ota_agent.d ./Core/Src/ota/ota_agent.o ./Core/Src/ota/ota_agent.su ./Core/Src/ota/ota_metadata.cyclo ./Core/Src/ota/ota_metadata.d ./Core/Src/ota/ota_metadata.o ./Core/Src/ota/ota_metadata.su

.PHONY: clean-Core-2f-Src-2f-ota

