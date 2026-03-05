#include <stdint.h>
#include "stm32f7xx_hal.h"
#include "flash_layout.h"

/*
 * Erase OTA slot sector
 */
void ota_flash_erase(uint32_t address)
{
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase;

    uint32_t sector_error;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_7;      // adjust if your slot uses another sector
    erase.NbSectors = 1;

    HAL_FLASHEx_Erase(&erase, &sector_error);

    HAL_FLASH_Lock();
}

/*
 * Write firmware chunk
 */
void ota_flash_write(uint32_t address, uint8_t *data, uint32_t length)
{
    HAL_FLASH_Unlock();

    for(uint32_t i = 0; i < length; i += 4)
    {
        uint32_t word = *(uint32_t*)(data + i);

        HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_WORD,
            address + i,
            word
        );
    }

    HAL_FLASH_Lock();
}
