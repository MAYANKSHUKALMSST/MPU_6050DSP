#include "stm32f7xx_hal.h"

void flash_write(uint32_t address, uint8_t *data, uint32_t len)
{
    HAL_FLASH_Unlock();

    for(uint32_t i=0;i<len;i+=8)
    {
        uint64_t value = *(uint64_t*)(data+i);

        HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_DOUBLEWORD,
            address+i,
            value);
    }

    HAL_FLASH_Lock();
}
