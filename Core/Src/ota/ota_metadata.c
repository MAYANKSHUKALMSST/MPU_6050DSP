/* ============================================================
 * ota/ota_metadata.c
 *
 * Confirms a completed OTA update by clearing the pending_update
 * flag in the metadata sector.  Called by the application after
 * it has successfully booted post-OTA.
 * ============================================================ */

#include "ota/ota_metadata.h"
#include "ota/flash_manager.h"
#include "main.h"   /* LD3_Pin (PB14), LD3_GPIO_Port (GPIOB) */

#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------
 * Blink LD3 (red LED, PB14) n times to signal OTA success.
 * Uses HAL_Delay — called after HAL_Init() has run.
 * ---------------------------------------------------------- */
static void ota_blink_success(uint8_t n)
{
    for (uint8_t i = 0U; i < n; i++) {
        HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
        HAL_Delay(150U);
        HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
        HAL_Delay(150U);
    }
}

void ota_confirm_update(void)
{
    /* Read current metadata from flash into RAM */
    ota_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    flash_read(OTA_METADATA_ADDR, (uint8_t *)&meta, sizeof(meta));

    /* Only act if there is a genuine pending update record */
    if (meta.magic != OTA_MAGIC || meta.pending_update != 1U)
    {
        printf("OTA: No pending update to confirm\n");
        return;
    }

    printf("OTA: Confirming update — marking firmware as stable\n");

    /* Clear the pending flag and reset the rollback counter */
    meta.pending_update  = 0U;
    meta.boot_fail_count = 0U;  /* firmware confirmed stable — reset rollback watchdog */

    /* Erase metadata sector then re-write with updated struct */
    if (flash_erase(OTA_METADATA_ADDR, FLASH_METADATA_SIZE) != 0)
    {
        printf("OTA: Metadata erase failed!\n");
        return;
    }

    flash_write(OTA_METADATA_ADDR, (uint8_t *)&meta, sizeof(meta));

    printf("OTA: Update confirmed successfully — blinking LD3 x6\n");

    /* Blink LD3 (red LED) 6 times: visible OTA success indicator */
    ota_blink_success(6U);
}
