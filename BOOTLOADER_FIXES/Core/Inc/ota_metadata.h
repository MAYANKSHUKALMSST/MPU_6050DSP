/* ============================================================
 * ota_metadata.h  (bootloader project)
 *
 * Kept in sync with the app project's ota/ota_metadata.h.
 * Both projects must have identical ota_metadata_t definitions
 * so the bootloader can read metadata written by the app.
 * ============================================================ */

#ifndef OTA_METADATA_H
#define OTA_METADATA_H

#include <stdint.h>
#include "flash_layout.h"

/* Magic value written to flash to identify valid metadata     */
#define OTA_MAGIC           0x4F544131UL   /* ASCII "OTA1"    */

typedef struct
{
    uint32_t magic;           /* Must equal OTA_MAGIC              */
    uint32_t active_slot;     /* 0 = Slot A, 1 = Slot B            */
    uint32_t pending_update;  /* 1 = new firmware waiting in Slot B*/
    uint32_t firmware_size;   /* Size in bytes of the new firmware */
    uint32_t firmware_crc;    /* Additive checksum of firmware     */

} ota_metadata_t;

/* Convenience pointer to the metadata sector in flash */
#define OTA_METADATA_ADDR   FLASH_METADATA_ADDR
#define OTA_META            ((ota_metadata_t *)OTA_METADATA_ADDR)

#endif /* OTA_METADATA_H */
