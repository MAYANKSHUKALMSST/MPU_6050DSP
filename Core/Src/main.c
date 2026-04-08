/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    main.c
 * @brief   Predictive Maintenance Firmware (Hardware, Bootstrapper & OTA)
 *          STM32F722ZE + MPU6050 + CMSIS-DSP + FreeRTOS
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "arm_math.h"
#include "mpu6050.h"
#include "ota/ota_metadata.h"  /* ota_confirm_update() — blinks LD3 on OTA boot */
#include <stdio.h>
#include <string.h>

/* IWDG is driven directly via registers (stm32f722xx.h) — no HAL driver needed
 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

typedef struct {
  volatile float ax, ay, az;
  volatile float temp;
  volatile float peak;
  volatile float freq;
  volatile int fault;
} TelemetryData_t;

volatile TelemetryData_t g_telemetry = {0};

/* Queue message: MPU6050_Data_t (28 bytes) + 4 bytes pad = exactly 32 bytes */
typedef struct {
  MPU6050_Data_t data;
  uint32_t pad;
} imu_msg_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

I2C_HandleTypeDef hi2c1;

UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart6;

PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* IWDG — ~4 second timeout, robust against LSI frequency variation.
 *
 * STM32F722 LSI range: 17–47 kHz (nominal 32 kHz).
 * Prescaler /128 → tick = 128/LSI.
 *   Nominal  (32 kHz): tick = 4.0 ms  → 1249 × 4.0 ms = 5.0 s  (max)
 *   Fast end (47 kHz): tick = 2.7 ms  → 1249 × 2.7 ms = 3.4 s  (min)
 *   Slow end (17 kHz): tick = 7.5 ms  → 1249 × 7.5 ms = 9.4 s  (max)
 *
 * defaultTask kicks every 500 ms (osPriorityRealtime), giving ≥6× margin
 * even at the fastest LSI edge.  Any genuine hang → reset in ≤3.4 s.    */
static inline void iwdg_init(void) {
  IWDG->KR = 0x5555U; /* unlock PR/RLR */
  IWDG->PR = 0x05U;   /* prescaler /128 */
  IWDG->RLR = 1249U;  /* ~4 s nominal, ≥3.4 s worst-case */
  IWDG->KR = 0xAAAAU; /* reload counter */
  IWDG->KR = 0xCCCCU; /* start watchdog */
}
static inline void iwdg_kick(void) { IWDG->KR = 0xAAAAU; }

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
    .name = "defaultTask",
    .stack_size = 128 * 4,
    /* Must be the HIGHEST priority task so the IWDG heartbeat can never be
     * starved by SensorTask (High) or DSPTask (AboveNormal).  This task only
     * calls iwdg_kick() and sleeps 500 ms, so its CPU cost is negligible.   */
    .priority = (osPriority_t)osPriorityRealtime,
};

/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
    .name = "SensorTask",
    .stack_size = 512 * 4,
    .priority = (osPriority_t)osPriorityHigh,
};

/* Definitions for DSPTask */
osThreadId_t DSPTaskHandle;
const osThreadAttr_t DSPTask_attributes = {
    .name = "DSPTask",
    .stack_size = 1024 * 4,
    .priority = (osPriority_t)osPriorityAboveNormal1,
};

/* Definitions for TelemetryTask */
osThreadId_t TelemetryTaskHandle;
const osThreadAttr_t TelemetryTask_attributes = {
    .name = "TelemetryTask",
    .stack_size = 512 * 4,
    .priority = (osPriority_t)osPriorityNormal,
};

/* Definitions for imuQueue */
osMessageQueueId_t imuQueueHandle;
const osMessageQueueAttr_t imuQueue_attributes = {.name = "imuQueue"};

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USART6_UART_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
void StartDefaultTask(void *argument);
void StartSensorTask(void *argument);
void StartDSPTask(void *argument);
void StartTelemetryTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#include <stdarg.h>

osMutexId_t printMutexHandle = NULL;
const osMutexAttr_t printMutex_attributes = {.name = "printMutex"};

void safe_printf(const char *fmt, ...) {
  /* If inside an interrupt (IPSR != 0) or OS not running, bypass the mutex */
  int is_isr = (__get_IPSR() != 0);
  int use_mutex = (!is_isr && osKernelGetState() == osKernelRunning &&
                   printMutexHandle != NULL);

  if (use_mutex) {
    osMutexAcquire(printMutexHandle, osWaitForever);
  }

  va_list args;
  va_start(args, fmt);
#undef printf
  vprintf(fmt, args);
#define printf safe_printf
  va_end(args);

  if (use_mutex) {
    osMutexRelease(printMutexHandle);
  }
}

/**
 * @brief  Retarget printf to USART3 (ST-Link VCP — same as bootloader).
 *         Called by the C standard library for every character output.
 */
int __io_putchar(int ch) {
  HAL_UART_Transmit(&huart3, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
  return ch;
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {
  /* USER CODE BEGIN 1 */

  /* ── LED ENTRY DIAGNOSTIC ────────────────────────────────────────────────
   * This runs before VTOR, before __enable_irq(), before UART — pure GPIO.
   * If green LD1 (PB0) blinks → main() IS entered on this warm reset.
   * If it never blinks         → fault is in startup code before main().
   *
   * Uses only GPIOB which cannot have been reset (AHB1RSTR touches GPIOBRST
   * but HAL_DeInit only pulsed FORCE/RELEASE — clock stays enabled).
   * Even if GPIOBEN was cleared by HAL_DeInit, we force it here first.     */
  RCC->AHB1ENR |= (1U << 1); /* GPIOBEN — force GPIOB clock on   */
  __DSB();                   /* ensure clock reaches GPIOB       */
  GPIOB->MODER = (GPIOB->MODER & ~(3U << 0)) | (1U << 0); /* PB0 = output */
  for (int _i = 0; _i < 6; _i++) {
    GPIOB->ODR ^= (1U << 0); /* toggle LD1 (green)               */
    for (volatile uint32_t _d = 0; _d < 160000UL; _d++) {
    } /* ~100 ms    */
  }
  GPIOB->ODR &= ~(1U << 0); /* leave LED off                    */
  /* ── END LED ENTRY DIAGNOSTIC ───────────────────────────────────────── */

  /* Relocate vector table to Slot A.
   * Do NOT call __enable_irq() here yet.  jump_to_application() did:
   *   1. NVIC->ICER[] (clear all IRQ enables)
   *   2. HAL_RCC_DeInit() → internally calls HAL_InitTick() which re-arms
   *      TIM6_DAC_IRQn in NVIC
   * Reset_Handler (just ran) zeroed BSS → htim6.Instance = NULL.
   * If we enable IRQs now, TIM6 fires instantly, HAL_TIM_IRQHandler()
   * dereferences htim6.Instance = NULL → HardFault → IWDG reboot loop.
   * IRQs are re-enabled below, after HAL_Init() sets htim6.Instance = TIM6. */
  SCB->VTOR = 0x08020000;
  __DSB();
  __ISB();

  /* ── BARE-METAL UART DIAGNOSTIC ─────────────────────────────────────────
   * Uses only volatile-counter timeouts — no IRQs or HAL needed.
   * LED error codes on LD1 (PB0) if a flag never sets:
   *   RAPID blink (~30 ms, infinite)  = hung at TXE: USART3 not starting.
   *   SLOW  blink (~200 ms, infinite) = hung at TC:  USART3 up, no TX done.
   *
   * After HAL_RCC_DeInit, PCLK1 = 16 MHz (HSI, no prescaler).
   * BRR = 16 000 000 / 115 200 ≈ 139 → correct 115 200 baud.              */
  RCC->AHB1ENR |= (1U << 3); /* GPIODEN  — force GPIOD clock on         */
  __DSB();
  RCC->APB1ENR |= (1U << 18); /* USART3EN — force USART3 clock on        */
  __DSB();

  GPIOD->MODER = (GPIOD->MODER & ~(3U << 16)) | (2U << 16);     /* PD8 = AF   */
  GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~(3U << 16)) | (3U << 16); /* PD8 = fast */
  GPIOD->AFR[1] = (GPIOD->AFR[1] & ~(0xFU << 0)) | (7U << 0);   /* PD8 = AF7  */
  USART3->BRR = 139U; /* 16 MHz / 139 = 115 108 baud (0.08 % error)  */
  USART3->CR1 = USART_CR1_TE | USART_CR1_UE;

  /* Wait TXE — timeout 2 M cycles → rapid blink if stuck */
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TXE)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 25000UL; _d++) {
          }
        }
      }
    }
  }

  USART3->TDR = (uint8_t)'B';

  /* Wait TC — timeout 2 M cycles → slow blink if stuck */
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TC)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 200000UL; _d++) {
          }
        }
      }
    }
  }
  /* Leave USART3 enabled — HAL_UART_Init() will fully reconfigure it     */

  /* USER CODE END 1 */

  /* MCU Configuration -------------------------------------------------------*/

  /* Reset of all peripherals, initialize Flash interface and SysTick */
  HAL_Init();

  /* ── DIAGNOSTIC BYTE 'C': sent after HAL_Init(), BEFORE __enable_irq()  ──
   * Terminal shows:   'B' only       → hang inside HAL_Init()
   *                   'BC' only      → hang caused by __enable_irq() (IRQ
   * storm) 'BCH'          → hang after 'H', in blue blink or SCC    */
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TXE)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 25000UL; _d++) {
          }
        }
      }
    }
  }
  USART3->TDR = (uint8_t)'C';
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TC)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 200000UL; _d++) {
          }
        }
      }
    }
  }

  /* ── CRITICAL: purge SysTick / PendSV before enabling any IRQs ────────────
   * Warm-reset scenario: the app was running FreeRTOS (SysTick active).
   * Pressing NRST should reset SysTick, but SCB->ICSR.PENDSTSET can carry a
   * latched count-flag edge that survives the reset cycle on some STM32F7
   * silicon revisions.  If __enable_irq() fires SysTick_Handler before
   * osKernelStart(), the FreeRTOS port calls xTaskIncrementTick() with
   * pxCurrentTCB=NULL (BSS zeroed by Reset_Handler) → NULL-deref in
   * PendSV_Handler → HardFault → while(1) → IWDG → green×3 loop.
   *
   * Fix: stop SysTick, clear both core-exception pending bits, wipe all NVIC
   * IRQ enables/pending, and re-arm only TIM6_DAC (HAL timebase).          */
  SysTick->CTRL = 0;                  /* disable SysTick counter     */
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk; /* clear SysTick pending bit   */
  SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk; /* clear PendSV  pending bit   */
  for (int _k = 0; _k < 8; _k++) {
    NVIC->ICER[_k] = 0xFFFFFFFFUL; /* disable every external IRQ  */
    NVIC->ICPR[_k] = 0xFFFFFFFFUL; /* clear  every pending bit    */
  }
  __DSB();
  __ISB();
  HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn); /* re-arm HAL timebase only    */

  /* htim6.Instance = TIM6 (set by HAL_Init). Only TIM6_DAC_IRQn is enabled.
   * No FreeRTOS SysTick / PendSV / DMA IRQ can fire before osKernelStart(). */
  __enable_irq();

  /* Send 'H' with timeouts — rapid blink = TXE stuck; slow blink = TC stuck */
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TXE)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 25000UL; _d++) {
          }
        }
      }
    }
  }
  USART3->TDR = (uint8_t)'H';
  {
    volatile uint32_t _t = 0;
    while (!(USART3->ISR & USART_ISR_TC)) {
      if (++_t > 2000000UL) {
        for (;;) {
          GPIOB->ODR ^= (1U << 0);
          for (volatile uint32_t _d = 0; _d < 200000UL; _d++) {
          }
        }
      }
    }
  }

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* ── DIAGNOSTIC: BLUE LED (PB7) blinks once — HAL_Init + irq + 'H' OK ──
   * Volatile loop at HSI 16 MHz: ~1 M cycles ≈ 62 ms per phase.
   * Observation guide:
   *   GREEN×3 only           → hang in USART TXE/TC bare-metal, or before
   *   GREEN×3 + BLUE×1       → SystemClock_Config() hung / Error_Handler
   *   GREEN×3 + BLUE×1 + RED×1 → hang AFTER SystemClock_Config()          */
  RCC->AHB1ENR |= (1U << 1); /* GPIOBEN — already on, safe to repeat */
  __DSB();
  GPIOB->MODER = (GPIOB->MODER & ~(3U << 14)) | (1U << 14); /* PB7 = output */
  GPIOB->ODR |= (1U << 7);
  for (volatile uint32_t _b = 0; _b < 3000000UL; _b++) {
  } /* ~187 ms at 16 MHz HSI */
  GPIOB->ODR &= ~(1U << 7);
  for (volatile uint32_t _b = 0; _b < 1000000UL; _b++) {
  } /* ~62 ms gap            */

  /* Switch to 168 MHz PLL (HSI source — no MCO/HSE settling dependency) */
  SystemClock_Config();

  /* ── DIAGNOSTIC: RED LED (PB14) blinks once — SystemClock_Config() OK ──
   * HAL_Delay() is now tick-accurate: HAL_RCC_ClockConfig recalibrated
   * TIM6 to PCLK1=42 MHz (prescaler=83, period=999 → 1 kHz).
   * If RED blink is absent: SCC called Error_Handler or hung.             */
  GPIOB->MODER = (GPIOB->MODER & ~(3U << 28)) | (1U << 28); /* PB14 = output */
  GPIOB->ODR |= (1U << 14);
  HAL_Delay(200);
  GPIOB->ODR &= ~(1U << 14);
  HAL_Delay(100);

  /* LED checkpoint: 2 short blinks after SystemClock_Config() returned.
   * Pattern: 3 rapid entry blinks, then 2 short blinks here.
   * If you see only 3 rapid → SystemClock_Config() hung.
   * HAL_Delay works here: __enable_irq() called above, htim6 valid.
   * Total: 2 × (50 ms ON + 50 ms OFF) = 200 ms added to boot.             */
  for (int _j = 0; _j < 2; _j++) {
    GPIOB->ODR |= (1U << 0);
    HAL_Delay(50);
    GPIOB->ODR &= ~(1U << 0);
    HAL_Delay(50);
  }

  /* Explicit USART3 peripheral reset before HAL init — guarantees the
   * peripheral comes up from a known-good hardware reset state regardless
   * of whether this is a cold boot or warm (NRST) reset.                  */
  __HAL_RCC_USART3_FORCE_RESET();
  __DSB();
  __HAL_RCC_USART3_RELEASE_RESET();
  __DSB();

  /* Single USART3 init at PCLK1 = 42 MHz (correct baud rate every time)  */
  MX_USART3_UART_Init();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize remaining peripherals */
  printf("\r\n=====================\r\n");
  printf("APPLICATION STARTED\r\n");
  printf("=====================\r\n");

  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_USART6_UART_Init();
  MX_I2C1_Init(); /* MPU6050 — includes I2C bus recovery */
  MX_USB_OTG_FS_PCD_Init();

  /* ---- OTA first-boot confirmation ----------------------------------------
   * If the bootloader just flashed slot B and jumped here for the first time,
   * pending_update == 1 in OTA metadata.  ota_confirm_update() clears the flag
   * and blinks LD3 (red LED) 6 times as a visual OTA-success indicator.
   * On normal (non-OTA) boots this is a cheap no-op (flash read + one printf).
   * Must be called AFTER MX_GPIO_Init() so GPIOB is configured.            */
  ota_confirm_update();

  /* USER CODE END 2 */

  /* Init FreeRTOS scheduler -------------------------------------------------*/
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  printMutexHandle = osMutexNew(&printMutex_attributes);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* Create message queue */
  imuQueueHandle = osMessageQueueNew(16, 32, &imuQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* Create threads */
  defaultTaskHandle =
      osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);
  DSPTaskHandle = osThreadNew(StartDSPTask, NULL, &DSPTask_attributes);
  TelemetryTaskHandle =
      osThreadNew(StartTelemetryTask, NULL, &TelemetryTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

  /* ---- Independent Watchdog: 2-second timeout ----
   * LSI clock ~32 kHz.  Prescaler /64 → tick = 2 ms.  Reload 999 → 2.0 s.
   * defaultTask kicks it every 500 ms; any hung task triggers a reset.   */
  iwdg_init();

  /* Start scheduler — does not return */
  osKernelStart();

  /* We should never reach here */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* ---------------------------------------------------------------------------*/
/* System Clock Configuration                                                 */
/* 168 MHz: HSI(16 MHz) → PLL (M=8, N=168, P=2, Q=7)                        */
/* SYSCLK=168 MHz  PCLK1=42 MHz  PCLK2=84 MHz  USB=48 MHz                   */
/* ≤180 MHz → no Overdrive required → no ODRDY/ODSWRDY timeout risk         */
/* ---------------------------------------------------------------------------*/
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /* Use HSI (16 MHz internal RC) as PLL source.
   * HSI is ready immediately after any reset — no MCO/HSE settling needed.
   * VCO input = 16 MHz / 8 = 2 MHz.
   *   VCO output = 2 × 168 = 336 MHz
   *   SYSCLK     = 336 / 2 = 168 MHz  (≤180 MHz — no Overdrive needed)
   *   USB clock  = 336 / 7 = 48 MHz   (exact)
   *   PCLK1      = 168 / 4 = 42 MHz
   *   PCLK2      = 168 / 2 = 84 MHz                                        */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;             /* 16 MHz / 8  = 2 MHz VCO in  */
  RCC_OscInitStruct.PLL.PLLN = 168;           /* 2 × 168     = 336 MHz VCO   */
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2; /* 336/2 = 168 MHz  */
  RCC_OscInitStruct.PLL.PLLQ = 7;             /* 336 / 7     = 48 MHz USB    */
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    Error_Handler();

  /* No HAL_PWREx_EnableOverDrive() — not required below 180 MHz.
   * Removing this call eliminates the ODRDY/ODSWRDY 1000 ms timeout that
   * caused silent Error_Handler() on warm reset when the bootloader's
   * HAL_DeInit() had reset the PWR peripheral via APB1 FORCE_RESET.        */

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  /* FLASH_LATENCY_5 = 5 wait states, correct for 150 < HCLK ≤ 180 MHz     */
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
    Error_Handler();
}

/* ---------------------------------------------------------------------------*/
/* I2C1 bus recovery — releases SDA if a slave holds it low after a reset.  */
/* Manually toggle SCL 9 times then issue a STOP (SDA LOW→HIGH while SCL    */
/* HIGH) using PB8/PB9 as plain GPIO outputs.                                */
/* ---------------------------------------------------------------------------*/
static void I2C1_BusRecovery(void) {
  GPIO_InitTypeDef g = {0};

  /* 1. Enable GPIOB clock (may already be on, safe to repeat) */
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 2. Temporarily drive SCL (PB8) and SDA (PB9) as open-drain outputs */
  g.Pin = GPIO_PIN_8 | GPIO_PIN_9;
  g.Mode = GPIO_MODE_OUTPUT_OD;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &g);

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9,
                    GPIO_PIN_SET); /* both HIGH */
  HAL_Delay(1);

  /* 3. Toggle SCL 9 times to release any stuck slave */
  for (int i = 0; i < 9; i++) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET); /* SCL LOW  */
    HAL_Delay(1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET); /* SCL HIGH */
    HAL_Delay(1);
  }

  /* 4. Issue STOP condition: SDA LOW while SCL HIGH, then SDA HIGH */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET); /* SDA LOW  */
  HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET); /* SCL HIGH */
  HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET); /* SDA HIGH */
  HAL_Delay(1);

  /* 5. De-init GPIOs — HAL_I2C_MspInit will re-configure them as AF4 */
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8 | GPIO_PIN_9);
}

/* ---------------------------------------------------------------------------*/
/* I2C1 Init — MPU6050 (400 kHz Fast Mode)                                   */
/* ---------------------------------------------------------------------------*/
static void MX_I2C1_Init(void) {
  /* USER CODE BEGIN I2C1_Init 0 */
  /* USER CODE END I2C1_Init 0 */

  /* Hard-reset the I2C1 peripheral before touching the bus.
   * This clears any residual BUSY/error state left in I2C1 hardware registers
   * after a warm reset, even before the GPIO pins are reconfigured.         */
  __HAL_RCC_I2C1_FORCE_RESET();
  __DSB();
  HAL_Delay(1); /* ≥1 APB cycle at any clock — generous     */
  __HAL_RCC_I2C1_RELEASE_RESET();
  __DSB();

  /* Recover bus first — prevents HAL_I2C_Init from failing when the MPU6050
   * holds SDA low after an unexpected reset (IWDG, power glitch, etc.)      */
  I2C1_BusRecovery();

  /* USER CODE BEGIN I2C1_Init 1 */
  /* USER CODE END I2C1_Init 1 */

  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x20404768;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    Error_Handler();
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
    Error_Handler();
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
    Error_Handler();

  /* USER CODE BEGIN I2C1_Init 2 */
  /* USER CODE END I2C1_Init 2 */
}

/* ---------------------------------------------------------------------------*/
/* USART2 Init — 115200 8N1 — Debug Terminal / printf                         */
/* ---------------------------------------------------------------------------*/
static void MX_USART2_UART_Init(void) {
  /* USER CODE BEGIN USART2_Init 0 */
  /* USER CODE END USART2_Init 0 */
  /* USER CODE BEGIN USART2_Init 1 */
  /* USER CODE END USART2_Init 1 */

  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
    Error_Handler();

  /* USER CODE BEGIN USART2_Init 2 */
  /* USER CODE END USART2_Init 2 */
}

/* ---------------------------------------------------------------------------*/
/* USART3 Init — 115200 8N1 — SIM800                                          */
/* ---------------------------------------------------------------------------*/
static void MX_USART3_UART_Init(void) {
  /* USER CODE BEGIN USART3_Init 0 */
  /* USER CODE END USART3_Init 0 */
  /* USER CODE BEGIN USART3_Init 1 */
  /* USER CODE END USART3_Init 1 */

  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
    Error_Handler();

  /* USER CODE BEGIN USART3_Init 2 */
  /* USER CODE END USART3_Init 2 */
}

/* ---------------------------------------------------------------------------*/
/* USART6 Init — 115200 8N1 — ESP32                                           */
/* ---------------------------------------------------------------------------*/
static void MX_USART6_UART_Init(void) {
  /* USER CODE BEGIN USART6_Init 0 */
  /* USER CODE END USART6_Init 0 */
  /* USER CODE BEGIN USART6_Init 1 */
  /* USER CODE END USART6_Init 1 */

  huart6.Instance = USART6;
  huart6.Init.BaudRate = 115200;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  huart6.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart6.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart6) != HAL_OK)
    Error_Handler();

  /* USER CODE BEGIN USART6_Init 2 */
  /* USER CODE END USART6_Init 2 */
}

/* ---------------------------------------------------------------------------*/
/* USB OTG FS Init */
/* ---------------------------------------------------------------------------*/
static void MX_USB_OTG_FS_PCD_Init(void) {
  /* USER CODE BEGIN USB_OTG_FS_Init 0 */
  /* USER CODE END USB_OTG_FS_Init 0 */
  /* USER CODE BEGIN USB_OTG_FS_Init 1 */
  /* USER CODE END USB_OTG_FS_Init 1 */

  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.battery_charging_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK)
    Error_Handler();

  /* USER CODE BEGIN USB_OTG_FS_Init 2 */
  /* USER CODE END USB_OTG_FS_Init 2 */
}

/* ---------------------------------------------------------------------------*/
/* GPIO Init — LEDs, User Button, USB Power Switch                            */
/* ---------------------------------------------------------------------------*/
static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, LD1_Pin | LD3_Pin | LD2_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(USB_PowerSwitchOn_GPIO_Port, USB_PowerSwitchOn_Pin,
                    GPIO_PIN_RESET);

  /* User Button */
  GPIO_InitStruct.Pin = USER_Btn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USER_Btn_GPIO_Port, &GPIO_InitStruct);

  /* LEDs */
  GPIO_InitStruct.Pin = LD1_Pin | LD3_Pin | LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USB Power Switch */
  GPIO_InitStruct.Pin = USB_PowerSwitchOn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_PowerSwitchOn_GPIO_Port, &GPIO_InitStruct);

  /* USB Over-Current Sense */
  GPIO_InitStruct.Pin = USB_OverCurrent_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USB_OverCurrent_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ── FreeRTOS Stack-Overflow Hook ────────────────────────────────────────────
 * Called when FreeRTOS detects that a task has overrun its stack.
 * configCHECK_FOR_STACK_OVERFLOW must be 1 or 2 in FreeRTOSConfig.h.
 *
 * We blink LD3 (red, PB14) rapidly and print the task name so the
 * offending task can be identified on the USART3 terminal.  The IWDG
 * will eventually reset the board.  Increase the task's stack_size in
 * osThreadAttr_t to fix the underlying issue.
 * ─────────────────────────────────────────────────────────────────────────── */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  printf("\r\n!!! STACK OVERFLOW in task: %s !!!\r\n", pcTaskName);
  printf("!!! Increase stack_size in osThreadAttr_t for this task. !!!\r\n");

  /* Blink LD3 red rapidly until IWDG resets */
  for (;;) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    for (volatile uint32_t d = 0; d < 80000UL; d++);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
    for (volatile uint32_t d = 0; d < 80000UL; d++);
  }
}

/* USER CODE END 4 */

/* ---------------------------------------------------------------------------*/
/* FreeRTOS Task Implementations */
/* ---------------------------------------------------------------------------*/

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument) {
  /* USER CODE BEGIN 5 */
  /* Dedicated watchdog heartbeat task.
   * USART2 RX polling removed — TelemetryTask now owns all ESP32
   * communication on USART2. Two tasks reading the same UART causes
   * byte theft and data loss.                                       */

  for (;;) {
    iwdg_kick();
    osDelay(500);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
 * @brief  Function implementing the SensorTask thread.
 *         Read MPU6050 accelerometer/gyro data and push to imuQueue.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument) {
  /* USER CODE BEGIN StartSensorTask */
  imu_msg_t msg;
  uint32_t printTick = 0;

  /* ---- Initialise the MPU6050 ---- */
  memset(&msg, 0, sizeof(msg));

  /* Auto-detect I2C address: try AD0-LOW (0xD0) then AD0-HIGH (0xD2).
   * HAL_I2C_IsDeviceReady probes with 2 retries, 10 ms timeout each.   */
  if (HAL_I2C_IsDeviceReady(&hi2c1, MPU6050_ADDR_AD0_LOW, 2, 10) == HAL_OK) {
    mpu6050_i2c_addr = MPU6050_ADDR_AD0_LOW;
    printf("SENSOR: MPU6050 found at 0x68 (AD0 LOW)\r\n");
  } else if (HAL_I2C_IsDeviceReady(&hi2c1, MPU6050_ADDR_AD0_HIGH, 2, 10) ==
             HAL_OK) {
    mpu6050_i2c_addr = MPU6050_ADDR_AD0_HIGH;
    printf("SENSOR: MPU6050 found at 0x69 (AD0 HIGH)\r\n");
  } else {
    /* Neither address responded — run I2C scan to help diagnose */
    printf("SENSOR: MPU6050 NOT found on I2C1 bus scan:\r\n");
    int found = 0;
    for (uint16_t a = 0x10; a <= 0xEE; a += 2) {
      if (HAL_I2C_IsDeviceReady(&hi2c1, a, 1, 5) == HAL_OK) {
        printf("  -> device ACK at 0x%02X\r\n", a >> 1);
        found++;
      }
    }
    if (!found)
      printf("  (no devices found — check SDA/SCL wiring and 3.3V power)\r\n");
    for (;;) {
      iwdg_kick();
      osDelay(1000);
    } /* stay alive, IWDG kept fed */
  }

  if (MPU6050_Init(&hi2c1) != HAL_OK) {
    printf("SENSOR: MPU6050 init FAILED after address detect — unexpected\r\n");
    for (;;) {
      iwdg_kick();
      osDelay(1000);
    }
  }

  printf("SENSOR: MPU6050 init OK  (WHO_AM_I = 0x%02X)\r\n",
         MPU6050_WhoAmI(&hi2c1));

  for (;;) {
    /* ---- Read all axes at 500 Hz (2 ms period) ---- */
    if (MPU6050_ReadAll(&hi2c1, &msg.data) == HAL_OK) {
      /* Push to DSP queue; drop sample if queue is full (non-blocking) */
      osMessageQueuePut(imuQueueHandle, &msg, 0U, 0U);

      /* Update global telemetry for Web Dashboard */
      g_telemetry.ax = msg.data.accel_x;
      g_telemetry.ay = msg.data.accel_y;
      g_telemetry.az = msg.data.accel_z;
      g_telemetry.temp = msg.data.temp_c;

      /* Print a summary twice per second (every 250 samples) */
      if (++printTick >= 250U) {
        printTick = 0U;
        printf("IMU | Ax=%6.3f  Ay=%6.3f  Az=%6.3f g"
               "  |  Gx=%7.2f  Gy=%7.2f  Gz=%7.2f deg/s"
               "  |  T=%.1f C\r\n",
               msg.data.accel_x, msg.data.accel_y, msg.data.accel_z,
               msg.data.gyro_x, msg.data.gyro_y, msg.data.gyro_z,
               msg.data.temp_c);
      }
    } else {
      printf("SENSOR: I2C read error\r\n");
      osDelay(500); /* back off on errors */
    }

    osDelay(2); /* 1000 ms / 500 Hz = 2 ms */
  }
  /* USER CODE END StartSensorTask */
}

/* USER CODE BEGIN Header_StartDSPTask */
/**
 * @brief  Function implementing the DSPTask thread.
 *         Consume imuQueue data, run CMSIS-DSP FFT, detect bearing faults.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDSPTask */
void StartDSPTask(void *argument) {
  /* USER CODE BEGIN StartDSPTask */

  /* ----------------------------------------------------------------
   * FFT parameters
   *   Sample rate : 500 Hz  (2 ms period in SensorTask)
   *   FFT size    : 256 points
   *   Resolution  : 500 / 256 ≈ 1.95 Hz per bin
   *   Nyquist     : 250 Hz  (max valid bin = 127)
   *   Fault band  : 10 – 200 Hz  (bins  5 – 102)
   *
   *   All bins stay below 127 — no buffer overflow.
   * ---------------------------------------------------------------- */
#define DSP_FFT_SIZE 256U
#define DSP_SAMPLE_RATE_HZ 500.0f
#define DSP_FAULT_BIN_LOW                                                      \
  ((uint32_t)(10.0f * DSP_FFT_SIZE / DSP_SAMPLE_RATE_HZ)) /*   5 */
#define DSP_FAULT_BIN_HIGH                                                     \
  ((uint32_t)(200.0f * DSP_FFT_SIZE / DSP_SAMPLE_RATE_HZ)) /* 102 */
#define DSP_FAULT_THRESHOLD 0.30f /* g — tune to your bearing/motor */

  static float32_t fftInput[DSP_FFT_SIZE];  /* time-domain accel_z samples */
  static float32_t fftOutput[DSP_FFT_SIZE]; /* RFFT complex output         */

  arm_rfft_fast_instance_f32 rfftInst;
  arm_rfft_fast_init_f32(&rfftInst, DSP_FFT_SIZE);

  imu_msg_t msg;
  uint32_t sampleIdx = 0U;

  printf("DSP:  FFT engine ready (%u-point, %u Hz, fault band 10-200 Hz)\r\n",
         DSP_FFT_SIZE, (unsigned)DSP_SAMPLE_RATE_HZ);

  for (;;) {
    /* Block indefinitely until SensorTask delivers a sample */
    if (osMessageQueueGet(imuQueueHandle, &msg, NULL, osWaitForever) != osOK)
      continue;

    /* Collect Z-axis acceleration into the input buffer */
    fftInput[sampleIdx++] = msg.data.accel_z;

    if (sampleIdx < DSP_FFT_SIZE)
      continue; /* buffer not yet full */

    sampleIdx = 0U;

    /* ---- Run forward real FFT ---- */
    arm_rfft_fast_f32(&rfftInst, fftInput, fftOutput, 0 /* forward */);

    /* ---- Find peak magnitude inside the bearing-fault band ----
     * arm_rfft_fast_f32 output format (for k >= 1):
     *   fftOutput[2*k]   = Re[k]
     *   fftOutput[2*k+1] = Im[k]
     * DC (k=0) is at [0], Nyquist at [1] — we skip both.
     * Our fault band starts at k=21 (10 Hz), well above 1, so this is safe.
     * ----------------------------------------------------------- */
    float32_t peakMag = 0.0f;
    uint32_t peakBin = DSP_FAULT_BIN_LOW;

    for (uint32_t k = DSP_FAULT_BIN_LOW; k < DSP_FAULT_BIN_HIGH; k++) {
      float32_t re = fftOutput[2U * k];
      float32_t im = fftOutput[2U * k + 1U];
      /* Scale by 2/N: two-sided → one-sided peak amplitude in g  */
      float32_t mag = sqrtf(re * re + im * im) * (2.0f / (float)DSP_FFT_SIZE);
      if (mag > peakMag) {
        peakMag = mag;
        peakBin = k;
      }
    }

    float peakFreq = (float)peakBin * DSP_SAMPLE_RATE_HZ / (float)DSP_FFT_SIZE;

    /* ---- Report result ---- */
    int fault_status = 0;
    if (peakMag > DSP_FAULT_THRESHOLD) {
      fault_status = 1;
      printf("DSP:  *** BEARING FAULT *** peak=%.3f g @ %.1f Hz\r\n", peakMag,
             peakFreq);
    } else {
      printf("DSP:  OK -- peak=%.3f g @ %.1f Hz\r\n", peakMag, peakFreq);
    }

    g_telemetry.peak = peakMag;
    g_telemetry.freq = peakFreq;
    g_telemetry.fault = fault_status;
  }
  /* USER CODE END StartDSPTask */
}

/* USER CODE BEGIN Header_StartTelemetryTask */
/**
 * @brief  Function implementing the TelemetryTask thread.
 *         Send FFT results / fault flags to ESP32 via USART6.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartTelemetryTask */
void StartTelemetryTask(void *argument) {
/* USER CODE BEGIN StartTelemetryTask */
#include "network_interface.h"
  extern void ota_agent_run(void);
  extern osMutexId_t printMutexHandle;

  printf("TEL: Connecting to Autonomous ESP32 Edge Core...\n");
  osDelay(2500);

  for (;;) {
    char json[128];

    if (printMutexHandle)
      osMutexAcquire(printMutexHandle, osWaitForever);
    int len = snprintf(json, sizeof(json),
                       "{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"temp\":%.1f,"
                       "\"peak\":%.3f,\"freq\":%.1f,\"fault\":%d}\n",
                       g_telemetry.ax, g_telemetry.ay, g_telemetry.az,
                       g_telemetry.temp, g_telemetry.peak, g_telemetry.freq,
                       g_telemetry.fault);
    if (printMutexHandle)
      osMutexRelease(printMutexHandle);

    if (network_send((uint8_t *)json, len) < 0) {
      printf("TEL: Output pipeline busy.\n");
    }

    /* Check for incoming commands from ESP32 (byte-by-byte, non-blocking).
     * HAL_UART_Receive with len=63 would block until 63 bytes arrive or
     * timeout, causing short commands like "REBOOT\n" to be missed.
     * FIX: Widened RX window from 50ms to 350ms so the STM32 is listening
     * for REBOOT commands ~87% of the time (was only 11%).               */
    {
      char cmd_buf[64] = {0};
      int cmd_len = 0;
      uint8_t ch;
      uint32_t t0 = HAL_GetTick();

      while (HAL_GetTick() - t0 < 350 && cmd_len < 63) {
        if (HAL_UART_Receive(&huart2, &ch, 1, 10) == HAL_OK) {
          if (ch == '\n') break;
          cmd_buf[cmd_len++] = (char)ch;
        }
      }

      if (cmd_len > 0) {
        cmd_buf[cmd_len] = '\0';
        if (strstr(cmd_buf, "REBOOT")) {
          printf("TEL: ESP32 REBOOT command — resetting to bootloader for OTA\n");
          HAL_Delay(100);
          NVIC_SystemReset();
        } else if (strstr(cmd_buf, "UPDATE")) {
          /* UPDATE is handled entirely by the ESP32 (ESP32_Edge_Dashboard):
           *  1. ESP32 downloads firmware binary from the cloud server via HTTP.
           *  2. ESP32 sends us REBOOT\n once the download is complete.
           *  3. NVIC_SystemReset() above puts us back in the bootloader.
           *  4. Bootloader receives the firmware from ESP32 via the 0x55
           *     binary protocol (doFlash) and writes it to Slot B.
           *
           * Do NOT call ota_agent_run() here — that is a different, ASCII-based
           * protocol (GET_META / GET_BIN) that the ESP32 does not implement.
           * Calling it would race with the ESP32 download and always fail with
           * "OTA: Version check failed".
           */
          printf("TEL: UPDATE received — ESP32 downloading firmware, will send REBOOT when ready\n");
        }
      }
    }

    osDelay(50);  /* Tight loop — most time spent in RX window above */
  }
  /* USER CODE END StartTelemetryTask */
}

/* ---------------------------------------------------------------------------*/
/* HAL Callbacks & Error Handler */
/* ---------------------------------------------------------------------------*/

/**
 * @brief  Period elapsed callback — drives HAL timebase via TIM6.
 * @param  htim: TIM handle
 * @retval None
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
  /* USER CODE BEGIN Callback 0 */
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  /* USER CODE END Callback 1 */
}

/**
 * @brief  Error Handler — print diagnostics then wait for IWDG reset.
 * @retval None
 */
void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* Force USART3 ready even if we're called before the normal init path.
   * MX_USART3_UART_Init() is idempotent — safe to call multiple times.  */
  MX_USART3_UART_Init();
  printf("\r\nERROR: Error_Handler reached — check SystemClock / peripheral "
         "init\r\n");
  /* Do NOT disable IRQs — keep SysTick alive for HAL_UART_Transmit */
  while (1) {
    /* IWDG fires after 2 seconds and resets the MCU cleanly */
  }
  /* USER CODE END Error_Handler_Debug */
}

/* ---------------------------------------------------------------------------*/
/* FreeRTOS Fault Callbacks */
/* ---------------------------------------------------------------------------*/

/**
 * @brief  FreeRTOS stack overflow hook.
 *         Called when a task writes past the end of its stack.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  (void)xTask;
  printf("FAULT: Stack overflow in task [%s] — waiting for watchdog reset\r\n",
         pcTaskName);
  while (1) {
  }
}

/**
 * @brief  FreeRTOS malloc failed hook.
 *         Called when pvPortMalloc() returns NULL.
 */
void vApplicationMallocFailedHook(void) {
  printf("FAULT: FreeRTOS heap exhausted — waiting for watchdog reset\r\n");
  while (1) {
  }
}

/* ---------------------------------------------------------------------------*/
/* HardFault Diagnostic Handler */
/* ---------------------------------------------------------------------------*/

/**
 * @brief  HardFault handler — prints fault address then waits for IWDG reset.
 *         The C wrapper receives the stacked register frame so we can print PC.
 */
void HardFault_Handler_C(uint32_t *sp) {
  printf("FAULT: HardFault!\r\n");
  printf("  PC  = 0x%08lX\r\n", sp[6]); /* stacked PC  */
  printf("  LR  = 0x%08lX\r\n", sp[5]); /* stacked LR  */
  printf("  R0  = 0x%08lX\r\n", sp[0]);
  printf("  CFSR= 0x%08lX\r\n", SCB->CFSR);
  printf("  HFSR= 0x%08lX\r\n", SCB->HFSR);
  while (1) {
  } /* IWDG resets MCU in 2 s */
}

/* Naked trampoline — saves MSP/PSP into R0, then calls the C handler */
__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile("TST   LR, #4          \n" /* test EXC_RETURN bit 2          */
                 "ITE   EQ               \n"
                 "MRSEQ R0, MSP          \n" /* EQ: fault in handler → MSP */
                 "MRSNE R0, PSP          \n" /* NE: fault in thread  → PSP */
                 "B     HardFault_Handler_C \n");
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the file and line of a failed assert_param().
 * @param  file: source file name
 * @param  line: assert_param error line number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
