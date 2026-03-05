#include "stm32f7xx.h"
#include "boot_manager.h"
#include "flash_layout.h"
#include "ota_metadata.h"
#include "secure_boot.h"
#include "boot_log.h"

typedef void (*app_entry_t)(void);

/* ======================================================= */
/* Jump to firmware */
/* ======================================================= */

void jump_to_application(uint32_t addr)
{
    uint32_t app_stack = *(volatile uint32_t*)addr;
    uint32_t app_reset = *(volatile uint32_t*)(addr + 4);

    boot_log("\r\nBOOT: Jumping to app at 0x%08lX\r\n", addr);
    boot_log("BOOT: Stack = 0x%08lX\r\n", app_stack);
    boot_log("BOOT: Reset = 0x%08lX\r\n", app_reset);

    __disable_irq();

    /* Relocate vector table */
    SCB->VTOR = addr;

    /* Set MSP */
    __set_MSP(app_stack);

    /* Jump to reset handler */
    app_entry_t entry = (app_entry_t)app_reset;

    entry();
}

/* ======================================================= */
/* Boot Manager */
/* ======================================================= */

void boot_manager_run(void)
{
    ota_metadata_t *meta = OTA_META;

    uint32_t slot_addr;

    boot_log("\r\nBOOT: Checking firmware metadata\r\n");

    /* Determine active slot */

    if(meta->active_slot == 0)
        slot_addr = FLASH_SLOT_A_ADDR;
    else
        slot_addr = FLASH_SLOT_B_ADDR;

    /* Check pending OTA update */

    if(meta->pending_update)
    {
        boot_log("BOOT: OTA update pending\r\n");

        uint32_t new_slot =
            (meta->active_slot == 0) ?
            FLASH_SLOT_B_ADDR :
            FLASH_SLOT_A_ADDR;

        boot_log("BOOT: Verifying new firmware...\r\n");

        if(verify_firmware(new_slot,
                           meta->firmware_size,
                           meta->firmware_hash))
        {
            boot_log("BOOT: Firmware verification OK\r\n");

            meta->active_slot ^= 1;
            meta->pending_update = 0;
        }
        else
        {
            boot_log("BOOT: Firmware verification FAILED\r\n");
        }
    }

    /* Jump to active slot */

    if(meta->active_slot == 0)
        slot_addr = FLASH_SLOT_A_ADDR;
    else
        slot_addr = FLASH_SLOT_B_ADDR;

    boot_log("BOOT: Starting slot %d\r\n", meta->active_slot);

    jump_to_application(slot_addr);
}
