/* ============================================================
 * flash_layout.h  —  Single source of truth for flash addresses
 *
 * STM32F722ZE  512 KB  Single-bank flash
 *
 * Sector map (hardware fixed):
 *   Sector 0 :  16 KB  @ 0x08000000
 *   Sector 1 :  16 KB  @ 0x08004000
 *   Sector 2 :  16 KB  @ 0x08008000
 *   Sector 3 :  16 KB  @ 0x0800C000
 *   Sector 4 :  64 KB  @ 0x08010000
 *   Sector 5 : 128 KB  @ 0x08020000
 *   Sector 6 : 128 KB  @ 0x08040000
 *   Sector 7 : 128 KB  @ 0x08060000
 *
 * Layout:
 *   Bootloader : Sectors 0-4  128 KB  0x08000000 - 0x0801FFFF
 *   Slot A     : Sector  5    128 KB  0x08020000 - 0x0803FFFF
 *   Slot B     : Sector  6    128 KB  0x08040000 - 0x0805FFFF
 *   Metadata   : Sector  7    128 KB  0x08060000 - 0x0807FFFF
 *
 * NOTE: Each slot occupies exactly ONE hardware sector so it
 *       can be erased independently without touching the other.
 * ============================================================ */

#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "stm32f7xx_hal.h"

/* ---- Bootloader ------------------------------------------ */
#define FLASH_BOOT_START        0x08000000UL
#define FLASH_BOOT_SIZE         (128UL * 1024UL)

/* ---- Application Slot A (active firmware) ---------------- */
#define FLASH_SLOT_A_ADDR       0x08020000UL
#define FLASH_SLOT_A_SECTOR     FLASH_SECTOR_5
#define FLASH_SLOT_A_SIZE       (128UL * 1024UL)

/* ---- Application Slot B (OTA staging) -------------------- */
#define FLASH_SLOT_B_ADDR       0x08040000UL
#define FLASH_SLOT_B_SECTOR     FLASH_SECTOR_6
#define FLASH_SLOT_B_SIZE       (128UL * 1024UL)

/* ---- Slot size (both slots are the same) ----------------- */
#define SLOT_SIZE               FLASH_SLOT_A_SIZE

/* ---- OTA Metadata (start of Sector 7) -------------------- */
#define FLASH_METADATA_ADDR     0x08060000UL
#define FLASH_METADATA_SECTOR   FLASH_SECTOR_7
#define FLASH_METADATA_SIZE     (4UL * 1024UL)

/* Alias used by legacy code */
#define OTA_METADATA_ADDR       FLASH_METADATA_ADDR

#endif /* FLASH_LAYOUT_H */
