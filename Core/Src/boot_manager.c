#include "stm32f7xx.h"
#include "boot_manager.h"
#include "flash_layout.h"
#include "ota_metadata.h"

#include "boot_log.h"
#include "stm32f7xx_hal.h"

typedef void (*app_entry_t)(void);

/* ======================================================= */
/* Validate firmware before jump */
/* ======================================================= */
static int is_valid_app(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t*)addr;
    uint32_t reset = *(volatile uint32_t*)(addr + 4);

    /* Stack must be inside SRAM */
    if(sp < 0x20000000 || sp > 0x20040000)
        return 0;

    /* Reset vector must point inside FLASH */
    if(reset < 0x08000000 || reset > 0x08100000)
        return 0;

    return 1;
}

/* ======================================================= */
/* Jump to firmware */
/* ======================================================= */
void jump_to_application(uint32_t addr)
{
    uint32_t app_stack = *(volatile uint32_t*)addr;
    uint32_t app_reset = *(volatile uint32_t*)(addr + 4);

    if(!is_valid_app(addr))
    {
        boot_log("\r\nBOOT: Invalid firmware at 0x%08lX\r\n", addr);
        return;
    }

    boot_log("\r\nBOOT: Jumping to app at 0x%08lX\r\n", addr);
    boot_log("BOOT: SLOT_A = 0x%08lX\r\n", FLASH_SLOT_A_ADDR);
    boot_log("BOOT: SLOT_B = 0x%08lX\r\n", FLASH_SLOT_B_ADDR);

    HAL_Delay(50);   /* Allow UART logs to flush */

    __disable_irq();

    /* Stop SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* Disable NVIC interrupts */
    for(uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* Reset HAL peripherals */
    HAL_DeInit();

    /* Clear fault status registers */
    SCB->CFSR = SCB->CFSR;
    SCB->HFSR = SCB->HFSR;
    SCB->DFSR = SCB->DFSR;

    /* Relocate vector table */
    SCB->VTOR = addr;

    __DSB();
    __ISB();

    /* Set MSP */
    __set_MSP(app_stack);

    /* Jump to reset handler */
    app_entry_t entry = (app_entry_t)app_reset;
    entry();

    while(1); /* Should never return */
}
