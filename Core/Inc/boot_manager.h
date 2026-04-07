/* ============================================================
 * boot_manager.h
 *
 * Declarations for the boot manager module.
 *
 * NOTE: Function *definitions* must never go in a header file.
 * The flash_erase / flash_read implementations that were
 * previously in this file have been moved to their correct
 * location: Core/Src/ota/flash_manager.c
 * ============================================================ */

#ifndef BOOT_MANAGER_H
#define BOOT_MANAGER_H

#include <stdint.h>

void boot_manager_start(void);
void jump_to_application(uint32_t addr);

#endif /* BOOT_MANAGER_H */
