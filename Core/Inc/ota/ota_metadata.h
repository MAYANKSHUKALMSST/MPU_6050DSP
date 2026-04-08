/* ============================================================
 * ota/ota_metadata.h
 *
 * OTA metadata layout stored at FLASH_METADATA_ADDR.
 *
 * This header is the authoritative definition of ota_metadata_t
 * and must be kept identical to Core/Inc/ota_metadata.h.
 * The root-level ota_metadata.h will be removed in a future
 * clean-up; all code should include this file going forward.
 * ============================================================ */

#ifndef OTA_METADATA_H
#define OTA_METADATA_H

#include "flash_layout.h"
#include <stdint.h>


/* Magic value written to flash to identify valid metadata     */
#define OTA_MAGIC 0x4F544131UL /* ASCII "OTA1"    */

typedef struct {
  uint32_t magic;            /* Must equal OTA_MAGIC              */
  uint32_t active_slot;      /* 0 = Slot A, 1 = Slot B            */
  uint32_t pending_update;   /* 1 = new firmware waiting in Slot B*/
  uint32_t firmware_size;    /* Size in bytes of the new firmware */
  uint8_t firmware_hash[32]; /* SHA-256 hash of the new firmware  */

  /* Secure boot — provisioned on first boot, verified on every boot.
   * All three fields are written atomically when slot_a_provisioned
   * is set to 1.  Once set they are preserved across OTA updates.  */
  uint32_t slot_a_provisioned; /* 1 = slot_a_hash is valid          */
  uint32_t slot_a_size;        /* bytes of Slot A covered by hash   */
  uint8_t  slot_a_hash[32];    /* SHA-256 of the trusted Slot A     */

  /* Rollback watchdog.
   * Bootloader increments this BEFORE jumping to the app.
   * ota_confirm_update() resets it to 0 on a successful post-OTA boot.
   * If boot_fail_count reaches OTA_MAX_BOOT_FAILURES the bootloader
   * gives up re-copying Slot B and halts → reflash via ST-Link required. */
  uint32_t boot_fail_count;    /* consecutive failed boots after OTA */

} ota_metadata_t;
/* sizeof(ota_metadata_t) = 4+4+4+4+32+4+4+32+4 = 92 bytes (23 words) */

#define OTA_MAX_BOOT_FAILURES 3U

/* Convenience pointer to the metadata sector in flash */
#define OTA_METADATA_ADDR FLASH_METADATA_ADDR
#define OTA_META ((ota_metadata_t *)OTA_METADATA_ADDR)

/* ---- Application API ---------------------------------------------------- */
/**
 * @brief  Call once early in main() after MX_GPIO_Init().
 *         Detects a first-boot after OTA (pending_update == 1), clears the
 *         flag in flash, and blinks LD3 (red LED, PB14) 6 times as a visual
 *         OTA-success indicator.  No-op on normal (non-OTA) boots.
 */
void ota_confirm_update(void);

#endif /* OTA_METADATA_H */
