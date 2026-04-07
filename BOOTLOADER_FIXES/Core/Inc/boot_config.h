/* ============================================================
 * boot_config.h  (bootloader project)
 *
 * All flash addresses are now defined in flash_layout.h which
 * is the single source of truth for both projects.
 *
 * This file provides backward-compatible aliases so existing
 * code that includes boot_config.h does not need to change.
 * ============================================================ */

#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include "flash_layout.h"

/* Backward-compatible aliases */
#define SLOT_A_ADDR      FLASH_SLOT_A_ADDR    /* 0x08020000 */
#define SLOT_B_ADDR      FLASH_SLOT_B_ADDR    /* 0x08040000 */
#define OTA_METADATA     FLASH_METADATA_ADDR  /* 0x08060000 */

/* Slot index constants */
#define SLOT_A           0
#define SLOT_B           1

#endif /* BOOT_CONFIG_H */
