/* ============================================================
 * ota/flash_manager.c
 *
 * Flash erase / write / read for STM32F722ZE (512 KB).
 *
 * Sector map:
 *   Sector 0 :  16 KB  @ 0x08000000
 *   Sector 1 :  16 KB  @ 0x08004000
 *   Sector 2 :  16 KB  @ 0x08008000
 *   Sector 3 :  16 KB  @ 0x0800C000
 *   Sector 4 :  64 KB  @ 0x08010000
 *   Sector 5 : 128 KB  @ 0x08020000  <- Slot A
 *   Sector 6 : 128 KB  @ 0x08040000  <- Slot B
 *   Sector 7 : 128 KB  @ 0x08060000  <- Metadata
 * ============================================================ */

#include "ota/flash_manager.h"
#include "flash_layout.h"
#include "stm32f7xx_hal.h"

#include <string.h>

/* ----------------------------------------------------------
   Map a flash address to its hardware sector number.
   Returns 0xFF if the address is outside flash.
---------------------------------------------------------- */
static uint32_t addr_to_sector(uint32_t addr)
{
    if (addr < 0x08004000UL) return FLASH_SECTOR_0;
    if (addr < 0x08008000UL) return FLASH_SECTOR_1;
    if (addr < 0x0800C000UL) return FLASH_SECTOR_2;
    if (addr < 0x08010000UL) return FLASH_SECTOR_3;
    if (addr < 0x08020000UL) return FLASH_SECTOR_4;
    if (addr < 0x08040000UL) return FLASH_SECTOR_5;
    if (addr < 0x08060000UL) return FLASH_SECTOR_6;
    if (addr < 0x08080000UL) return FLASH_SECTOR_7;
    return 0xFFU;
}

/* ----------------------------------------------------------
   flash_erase
   Erases every hardware sector touched by the range
   [addr, addr+size).  VOLTAGE_RANGE_3 assumes Vcc 2.7-3.6V
   which allows 32-bit parallelism.

   Returns  0 on success, -1 on failure.
---------------------------------------------------------- */
int flash_erase(uint32_t addr, uint32_t size)
{
    if (size == 0U)
        return 0;

    uint32_t first = addr_to_sector(addr);
    uint32_t last  = addr_to_sector(addr + size - 1U);

    if (first == 0xFFU || last == 0xFFU)
        return -1;

    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector       = first;
    erase.NbSectors    = last - first + 1U;

    uint32_t sector_error = 0U;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? 0 : -1;
}

/* ----------------------------------------------------------
   flash_write
   Programs [size] bytes from [data] into flash at [address].

   STM32F7 requires 8-byte (doubleword) aligned writes.
   Any partial final block is padded with 0xFF before writing.

   IMPORTANT: The target region must be erased before calling.
---------------------------------------------------------- */
void flash_write(uint32_t address, uint8_t *data, uint32_t size)
{
    if (size == 0U || data == NULL)
        return;

    HAL_FLASH_Unlock();

    uint32_t i = 0U;

    /* Write full 8-byte doublewords */
    while (i + 8U <= size)
    {
        uint64_t word;
        memcpy(&word, data + i, 8U);

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          address + i,
                          word);
        i += 8U;
    }

    /* Handle remaining 1-7 bytes — pad with 0xFF (erased value) */
    if (i < size)
    {
        uint8_t tail[8];
        memset(tail, 0xFFU, sizeof(tail));
        memcpy(tail, data + i, size - i);

        uint64_t word;
        memcpy(&word, tail, 8U);

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          address + i,
                          word);
    }

    HAL_FLASH_Lock();
}

/* ----------------------------------------------------------
   flash_read
   Copies [size] bytes from flash at [address] into [buffer].
   Flash is memory-mapped on Cortex-M so this is a plain memcpy.
---------------------------------------------------------- */
void flash_read(uint32_t address, uint8_t *buffer, uint32_t size)
{
    if (buffer == NULL || size == 0U)
        return;

    memcpy(buffer, (const void *)address, size);
}
