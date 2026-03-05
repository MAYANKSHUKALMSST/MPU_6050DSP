#ifndef OTA_METADATA_H
#define OTA_METADATA_H

#include <stdint.h>

#define OTA_MAGIC 0xDEADBEEF

typedef struct
{
    uint32_t magic;

    uint32_t active_slot;

    uint32_t pending_update;

    uint32_t confirmed;

    uint32_t firmware_size;

    uint8_t firmware_hash[32];

} ota_metadata_t;

#define OTA_META ((ota_metadata_t*)FLASH_METADATA_ADDR)

#endif
