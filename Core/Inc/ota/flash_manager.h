#ifndef FLASH_MANAGER_H
#define FLASH_MANAGER_H

#include <stdint.h>

int flash_erase(uint32_t addr, uint32_t size);
void flash_write(uint32_t addr, uint8_t *data, uint32_t size);
void flash_read(uint32_t addr, uint8_t *buffer, uint32_t size);

#endif
