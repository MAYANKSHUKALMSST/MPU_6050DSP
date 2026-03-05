#include "boot_log.h"
#include "stm32f7xx_hal.h"
#include <string.h>
extern UART_HandleTypeDef huart3;

void boot_log(const char *fmt, ...)
{
    char buffer[256];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    HAL_UART_Transmit(&huart3, (uint8_t*)buffer, strlen(buffer), 100);
}
