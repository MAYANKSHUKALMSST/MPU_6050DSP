/* ============================================================
 * boot_manager.c
 *
 * Reads OTA metadata from flash and selects the correct slot
 * to boot from, then performs a clean jump to the application.
 *
 * Boot logic:
 *   1. Read metadata sector
 *   2. If OTA_MAGIC + pending_update=1  → try Slot B first
 *   3. Otherwise                        → boot Slot A
 *   4. If chosen slot is invalid        → try the other slot
 *   5. If both invalid                  → hang (blink in main)
 * ============================================================ */

#include "stm32f7xx.h"
#include "stm32f7xx_hal.h"
#include "boot_manager.h"
#include "flash_layout.h"
#include "ota_metadata.h"

#include <string.h>
#include <stdio.h>

typedef void (*app_entry_t)(void);

/* ----------------------------------------------------------
   Validate application vector table
   Checks that the stack pointer is inside SRAM and the reset
   vector is inside flash — minimum sanity for a valid image.
---------------------------------------------------------- */
static int is_valid_app(uint32_t addr)
{
    uint32_t stack = *(volatile uint32_t *)addr;
    uint32_t reset = *(volatile uint32_t *)(addr + 4);

    /* Stack pointer must be inside SRAM (256 KB on STM32F722) */
    if (stack < 0x20000000UL || stack > 0x20040000UL)
        return 0;

    /* Reset handler must point inside flash */
    if (reset < 0x08000000UL || reset > 0x08080000UL)
        return 0;

    return 1;
}

/* ----------------------------------------------------------
   Perform a clean jump to the application
   Disables all interrupts, deinitialises HAL/RCC, relocates
   the vector table, sets MSP and branches to reset handler.
---------------------------------------------------------- */
void jump_to_application(uint32_t addr)
{
    uint32_t app_stack = *(volatile uint32_t *)addr;
    uint32_t app_reset = *(volatile uint32_t *)(addr + 4);

    printf("\r\nBOOT: Jumping to application at 0x%08X\r\n", (unsigned int)addr);
    printf("BOOT: Stack  = 0x%08X\r\n", (unsigned int)app_stack);
    printf("BOOT: Reset  = 0x%08X\r\n", (unsigned int)app_reset);

    HAL_Delay(100);   /* Allow UART output to flush */

    /* ---- Disable all interrupts ---- */
    __disable_irq();

    /* ---- Stop SysTick ---- */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* ---- Clear all NVIC enables and pending bits ---- */
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* ---- Reset RCC then full HAL deinit ---- */
    HAL_RCC_DeInit();
    HAL_DeInit();

    /* ---- Relocate vector table to app ---- */
    SCB->VTOR = addr;
    __DSB();
    __ISB();

    /* ---- Set stack pointer and jump ---- */
    __set_MSP(app_stack);

    app_entry_t app = (app_entry_t)app_reset;
    app();

    /* Should never reach here */
    while (1);
}

/* ----------------------------------------------------------
   Boot manager — entry point called from main()
---------------------------------------------------------- */
void boot_manager_start(void)
{
    printf("BOOT: Boot manager started\r\n");
    printf("BOOT: SLOT_A   = 0x%08X\r\n", (unsigned int)FLASH_SLOT_A_ADDR);
    printf("BOOT: SLOT_B   = 0x%08X\r\n", (unsigned int)FLASH_SLOT_B_ADDR);
    printf("BOOT: METADATA = 0x%08X\r\n", (unsigned int)FLASH_METADATA_ADDR);

    /* ---- Step 1: Read OTA metadata from flash ---- */
    ota_metadata_t meta;
    memcpy(&meta, (const void *)FLASH_METADATA_ADDR, sizeof(meta));

    printf("BOOT: Meta magic=0x%08X  pending=%lu  slot=%lu\r\n",
           (unsigned int)meta.magic,
           (unsigned long)meta.pending_update,
           (unsigned long)meta.active_slot);

    /* ---- Step 2: Choose boot slot ---- */
    uint32_t boot_addr = FLASH_SLOT_A_ADDR;   /* default */

    if (meta.magic == OTA_MAGIC && meta.pending_update == 1U)
    {
        printf("BOOT: Pending OTA update detected\r\n");

        if (is_valid_app(FLASH_SLOT_B_ADDR))
        {
            printf("BOOT: Slot B valid — selecting Slot B\r\n");
            boot_addr = FLASH_SLOT_B_ADDR;
        }
        else
        {
            printf("BOOT: Slot B invalid — ignoring OTA, keeping Slot A\r\n");
        }
    }
    else
    {
        printf("BOOT: No pending update — normal boot from Slot A\r\n");
    }

    /* ---- Step 3: Validate chosen slot ---- */
    if (!is_valid_app(boot_addr))
    {
        printf("BOOT: Chosen slot 0x%08X invalid, trying fallback\r\n",
               (unsigned int)boot_addr);

        uint32_t fallback = (boot_addr == FLASH_SLOT_A_ADDR)
                            ? FLASH_SLOT_B_ADDR
                            : FLASH_SLOT_A_ADDR;

        if (is_valid_app(fallback))
        {
            printf("BOOT: Fallback slot 0x%08X valid\r\n", (unsigned int)fallback);
            boot_addr = fallback;
        }
        else
        {
            /* Nothing to boot — signal error via LED blink in main() */
            printf("BOOT: *** NO VALID FIRMWARE FOUND — HALTING ***\r\n");
            while (1);
        }
    }

    printf("BOOT: Final boot address = 0x%08X\r\n", (unsigned int)boot_addr);
    HAL_Delay(50);

    jump_to_application(boot_addr);

    /* If jump_to_application() somehow returns, fall through to main's LED blink */
}
