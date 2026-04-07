# STM32F722ZE Predictive Maintenance Firmware — Project Notes

## Hardware
- Board: NUCLEO-F722ZE (STM32F722ZE Cortex-M7)
- Sensor: MPU-6050 on I2C1 (PB8=SCL, PB9=SDA)
- UARTs: USART3 = ST-Link VCP (printf), USART2 = ESP32, USART6 = SIM800
- LEDs: PB0=LD1 green, PB7=LD2 blue, PB14=LD3 red

## Flash Layout
| Region      | Base        | Size  |
|-------------|-------------|-------|
| Bootloader  | 0x08000000  | 128 KB |
| Slot A (app)| 0x08020000  | 256 KB |

## Clock Configuration — RESOLVED (168 MHz, no Overdrive)
**App SystemClock_Config: HSI → PLL → 168 MHz**
- PLLM=8, PLLN=168, PLLP=2, PLLQ=7
- SYSCLK=168 MHz, PCLK1=42 MHz, PCLK2=84 MHz, USB=48 MHz
- Flash latency: WS5 (FLASH_LATENCY_5)
- **NO `HAL_PWREx_EnableOverDrive()`** — not needed below 180 MHz

**Bootloader SystemClock_Config: HSI → PLL → 216 MHz**
- PLLM=8, PLLN=216, PLLP=2, PLLQ=9 — still uses EnableOverDrive (works at cold/NRST boot)
- Note: Bootloader calls HAL_RCC_DeInit() before jumping to app, switching system to HSI 16 MHz, so PLL mismatch between bootloader (216) and app (168) is harmless.

## HAL Timebase
Both bootloader and app use **TIM6** as HAL tick source (NOT SysTick).
- File: `Core/Src/stm32f7xx_hal_timebase_tim.c`
- FreeRTOS uses SysTick separately via `xPortSysTickHandler` = `SysTick_Handler`

## IWDG Configuration
Both bootloader and app: PR=/128 (0x05), RLR=1249
- Nominal timeout ~5 s at LSI=32 kHz
- Bootloader kicks every 500 ms during 3 s OTA window
- App defaultTask (osPriorityRealtime) kicks every 500 ms
- `iwdg_init()` called just before `osKernelStart()`

---

## ✅ SOLVED: Warm Reset (NRST) Failure — "green×3 only, no sensor readings"

### Root Cause 1 — HAL_PWREx_EnableOverDrive() timeout (PRIMARY)
**Symptom:** App worked on cold boot, failed on every NRST press (green×3, no output).

**Mechanism:**
1. App was running at 216 MHz with Overdrive enabled.
2. User pressed NRST → bootloader ran → jumped to app via `jump_to_application()`.
3. `jump_to_application()` calls `HAL_DeInit()` which does APB1 `FORCE_RESET` → this reset the PWR peripheral, clearing `VOSRDY` and `ODRDY` flags.
4. App's `SystemClock_Config()` then called `HAL_PWREx_EnableOverDrive()` which polls `ODRDY` with a 1000 ms timeout → timed out → `Error_Handler()` → `while(1)` → IWDG reset → green×3 again → loop.

**Fix:** Changed app clock to 168 MHz (≤180 MHz threshold → no Overdrive required).
```c
// Before (broken):   PLLN=216, PLLQ=9, FLASH_LATENCY_7, HAL_PWREx_EnableOverDrive()
// After  (fixed):    PLLN=168, PLLQ=7, FLASH_LATENCY_5,  NO EnableOverDrive
```

---

### Root Cause 2 — FreeRTOS SysTick firing before osKernelStart() (SECONDARY)
**Symptom:** After fix 1, terminal showed `BC` but not `H`; no blue LED on warm reset.

**Mechanism:**
1. FreeRTOS's `SysTick_Handler` (= `xPortSysTickHandler` from port.c) calls `xTaskIncrementTick()` unconditionally — no scheduler-started guard.
2. On warm reset, a latched SysTick pending bit (or TIM6_DAC IRQ firing) could trigger `SysTick_Handler` after `__enable_irq()` but before `osKernelStart()`.
3. `pxCurrentTCB = NULL` (BSS zeroed by Reset_Handler) → NULL-deref in `PendSV_Handler` → HardFault → `while(1)` → IWDG → green×3 loop.

**Fix:** Before `__enable_irq()`, added in `main()`:
```c
SysTick->CTRL = 0;                        // disable SysTick counter
SCB->ICSR     = SCB_ICSR_PENDSTCLR_Msk;  // clear SysTick pending
SCB->ICSR     = SCB_ICSR_PENDSVCLR_Msk;  // clear PendSV pending
for (int k = 0; k < 8; k++) {
    NVIC->ICER[k] = 0xFFFFFFFFUL;        // disable all external IRQs
    NVIC->ICPR[k] = 0xFFFFFFFFUL;        // clear all pending bits
}
__DSB(); __ISB();
HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);       // re-arm HAL timebase only
__enable_irq();
```

---

### Additional Hardening Applied
- **USART3 reset before HAL init:** `__HAL_RCC_USART3_FORCE_RESET()` / `RELEASE_RESET()` before `MX_USART3_UART_Init()` — guarantees peripheral starts from known state.
- **I2C1 reset before bus recovery:** `__HAL_RCC_I2C1_FORCE_RESET()` / `RELEASE_RESET()` + `HAL_Delay(1)` inside `MX_I2C1_Init()`, before `I2C1_BusRecovery()` + `HAL_I2C_Init()` — clears any stuck I2C hardware state from a mid-transaction NRST.
- **I2C1 bus recovery:** 9 SCL pulses + STOP condition on PB8/PB9 before `HAL_I2C_Init()` — releases MPU-6050 SDA if it was holding it low mid-transaction.
- **Blue blink with volatile loops:** Replaced `HAL_Delay()` calls before `SystemClock_Config()` with `for (volatile uint32_t _b = 0; ...)` loops — no TIM6 dependency before clock switch.

---

## Boot-time Diagnostics (main.c)
```
GREEN × 3   → app entered main()
'B' on UART → bare-metal USART3 TX working at HSI 16 MHz
'C' on UART → HAL_Init() completed
'H' on UART → SysTick/NVIC cleared, __enable_irq() done
BLUE × 1    → HAL init section complete, about to call SystemClock_Config()
RED  × 1    → SystemClock_Config() returned OK (HAL_Delay works = TIM6 at 168 MHz)
GREEN × 2   → checkpoints after clock config
"APPLICATION STARTED" → MX_USART3_UART_Init() succeeded, printf active
```
If `GREEN × 3` then nothing → hang in bare-metal USART or HAL_Init.
If `GREEN × 3` + `BLUE` but no `RED` → SystemClock_Config() hung.
If `GREEN × 3` + `BLUE` + `RED` but no "APPLICATION STARTED" → hang in peripheral init.

## FreeRTOS Tasks
| Task | Priority | Stack | Purpose |
|------|----------|-------|---------|
| defaultTask | Realtime | 512 B | IWDG kick every 500 ms + USART2 RX check |
| SensorTask | High | 2 KB | MPU-6050 read at 500 Hz, push to imuQueue |
| DSPTask | AboveNormal+1 | 4 KB | 256-pt RFFT, fault detection 10–200 Hz |
| TelemetryTask | Normal | 2 KB | USART6/ESP32 telemetry |

---

## ✅ SOLVED: Secure Boot — Slot A SHA-256 Verification

### Problem
`verify_firmware()` (SHA-256) was only called on the OTA path (Slot B, when `pending_update == 1`).
Normal Slot A boots only checked the vector table (`is_valid_app()`), leaving Slot A unverified.

### Solution: First-Boot Provisioning

**`ota_metadata_t`** extended with three new fields (appended, backwards-compatible):
```c
uint32_t slot_a_provisioned; /* 1 = slot_a_hash is valid          */
uint32_t slot_a_size;        /* bytes of Slot A hashed (=128 KB)  */
uint8_t  slot_a_hash[32];    /* SHA-256 of the trusted Slot A     */
```
Total struct size: 88 bytes (22 words × 4 — still properly aligned).
**Both `ota_metadata.h` files must stay identical** (bootloader and app).

**Boot flow after change (`boot_manager_start`):**
1. Read metadata from 0x08060000.
2. **First-boot provisioning** — if `slot_a_provisioned != 1`: compute SHA-256 of entire Slot A sector (128 KB), write back to metadata. Prints hash to UART for host-side verification.
3. OTA path unchanged: if `pending_update == 1`, verify Slot B (SHA-256) → boot Slot B.
4. **Normal path**: verify Slot A SHA-256 against `slot_a_hash` → boot if match, **halt** if mismatch (red LED blink loop, IWDG resets, keeps halting → requires ST-Link reflash).
5. `uart_ota.c` `metadata_write()` now reads existing metadata before erase and writes back `slot_a_provisioned/size/hash` so provisioning survives OTA commits.

### First-Boot UART output to expect
```
BOOT: First-boot provisioning — computing Slot A hash...
BOOT: Slot A SHA-256 = <64 hex chars>
BOOT: Slot A provisioned OK
BOOT: Verifying Slot A SHA-256 (131072 bytes)...
BOOT: Slot A hash OK
BOOT: Final boot address = 0x08020000
```
Subsequent boots skip provisioning:
```
BOOT: Meta magic=0x4F544131  pending=0  slot=0  provisioned=1
BOOT: No pending update — normal boot from Slot A
BOOT: Verifying Slot A SHA-256 (131072 bytes)...
BOOT: Slot A hash OK
```

### Files Changed
- `ota_bootloader/Core/Inc/ota_metadata.h` — 3 new fields
- `MPU_6050DSP/Core/Inc/ota/ota_metadata.h` — identical change
- `ota_bootloader/Core/Src/boot_manager.c` — provisioning + verification
- `ota_bootloader/Core/Src/uart_ota.c` — preserve slot_a fields in metadata_write()
