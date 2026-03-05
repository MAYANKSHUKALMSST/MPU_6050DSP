/* USER CODE BEGIN Header */
/**
******************************************************************************
* File Name          : freertos.c
* Description        : FreeRTOS vibration monitoring pipeline with OTA
*
* Pipeline:
*
* SensorTask    -> Acquire MPU6050 data
* DSPTask       -> FFT + vibration analysis
* TelemetryTask -> UART JSON streaming
* OtaTask       -> Background OTA polling
*
******************************************************************************
*/
/* USER CODE END Header */

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "main.h"
#include "mpu6050.h"
#include "arm_math.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "boot_log.h"
#include "ota_agent.h"

/* External peripherals */

extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart3;

/* ================= CONFIG ================= */

#define FFT_SIZE        256
#define SAMPLE_RATE     1000

/* ================= FFT BUFFERS ================= */

static float32_t fft_input[FFT_SIZE];
static float32_t fft_output[FFT_SIZE];
static float32_t fft_mag[FFT_SIZE/2];

static arm_rfft_fast_instance_f32 fft_handler;

/* ================= TASK HANDLES ================= */

osThreadId_t SensorTaskHandle;
osThreadId_t DSPTaskHandle;
osThreadId_t TelemetryTaskHandle;
osThreadId_t OtaTaskHandle;

/* ================= QUEUES ================= */

osMessageQueueId_t imuRawQueueHandle;
osMessageQueueId_t dspQueueHandle;

/* ================= DATA STRUCTURE ================= */

typedef struct
{
    float rms;
    float dominant_freq;
    float temperature;
    int health;

} DSP_Output_t;

/* ================= FUNCTION PROTOTYPES ================= */

void StartSensorTask(void *argument);
void StartDSPTask(void *argument);
void StartTelemetryTask(void *argument);
void StartOtaTask(void *argument);

/* OTA placeholders */

int check_update_available(void)
{
    return 0;
}

void download_firmware(void)
{
}

/* ===================================================== */
/* ================= FREERTOS INIT ===================== */
/* ===================================================== */

void MX_FREERTOS_Init(void)
{
    boot_log("RTOS: Initializing queues\r\n");

    imuRawQueueHandle =
        osMessageQueueNew(256, sizeof(MPU6050_Data_t), NULL);

    dspQueueHandle =
        osMessageQueueNew(16, sizeof(DSP_Output_t), NULL);

    boot_log("RTOS: Creating tasks\r\n");

    const osThreadAttr_t SensorTask_attributes =
    {
        .name = "SensorTask",
        .stack_size = 1024,
        .priority = osPriorityHigh,
    };

    SensorTaskHandle =
        osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

    const osThreadAttr_t DSPTask_attributes =
    {
        .name = "DSPTask",
        .stack_size = 2048,
        .priority = osPriorityAboveNormal,
    };

    DSPTaskHandle =
        osThreadNew(StartDSPTask, NULL, &DSPTask_attributes);

    const osThreadAttr_t TelemetryTask_attributes =
    {
        .name = "TelemetryTask",
        .stack_size = 1024,
        .priority = osPriorityNormal,
    };

    TelemetryTaskHandle =
        osThreadNew(StartTelemetryTask, NULL, &TelemetryTask_attributes);

    const osThreadAttr_t OtaTask_attributes =
    {
        .name = "OtaTask",
        .stack_size = 2048,
        .priority = osPriorityLow,
    };

    OtaTaskHandle =
        osThreadNew(StartOtaTask, NULL, &OtaTask_attributes);

    boot_log("RTOS: Tasks created successfully\r\n");
}

/* ===================================================== */
/* ================= SENSOR TASK ======================== */
/* ===================================================== */

void StartSensorTask(void *argument)
{
    boot_log("SENSOR TASK STARTED\r\n");

    MPU6050_Data_t imu;

    osDelay(200);

    if(HAL_I2C_IsDeviceReady(&hi2c1, 0x68 << 1, 3, 100) != HAL_OK)
    {
        boot_log("MPU6050 NOT DETECTED\r\n");
    }
    else
    {
        boot_log("MPU6050 DETECTED\r\n");
    }

    if(MPU6050_Init(&hi2c1) != HAL_OK)
    {
        boot_log("MPU INIT FAILED\r\n");
        vTaskSuspend(NULL);
    }

    boot_log("MPU INIT OK\r\n");

    for(;;)
    {
        if(MPU6050_ReadAll(&hi2c1, &imu) == HAL_OK)
        {
            osMessageQueuePut(imuRawQueueHandle, &imu, 0, 0);
        }

        osDelay(1);
    }
}

/* ===================================================== */
/* ================= DSP TASK =========================== */
/* ===================================================== */

void StartDSPTask(void *argument)
{
    boot_log("DSP TASK STARTED\r\n");

    MPU6050_Data_t imu;
    DSP_Output_t dsp;

    int sample_index = 0;

    arm_rfft_fast_init_f32(&fft_handler, FFT_SIZE);

    for(;;)
    {
        if(osMessageQueueGet(
                imuRawQueueHandle,
                &imu,
                NULL,
                osWaitForever) == osOK)
        {
            float accel_mag =
                sqrtf(
                    imu.accel_x * imu.accel_x +
                    imu.accel_y * imu.accel_y +
                    imu.accel_z * imu.accel_z);

            fft_input[sample_index++] = accel_mag;

            if(sample_index >= FFT_SIZE)
            {
                sample_index = 0;

                arm_rfft_fast_f32(
                    &fft_handler,
                    fft_input,
                    fft_output,
                    0);

                float max_val = 0;
                int max_idx = 0;

                for(int i = 1; i < FFT_SIZE / 2; i++)
                {
                    float real = fft_output[2 * i];
                    float imag = fft_output[2 * i + 1];

                    float mag =
                        sqrtf(real * real + imag * imag);

                    if(mag > max_val)
                    {
                        max_val = mag;
                        max_idx = i;
                    }
                }

                dsp.dominant_freq =
                    ((float)max_idx * SAMPLE_RATE) / FFT_SIZE;

                dsp.rms = max_val;
                dsp.temperature = imu.temp_c;

                if(dsp.rms < 0.2f)
                    dsp.health = 0;
                else if(dsp.rms < 0.5f)
                    dsp.health = 1;
                else
                    dsp.health = 2;

                osMessageQueuePut(dspQueueHandle, &dsp, 0, 0);
            }
        }
    }
}

/* ===================================================== */
/* ================= TELEMETRY TASK ===================== */
/* ===================================================== */

void StartTelemetryTask(void *argument)
{
    boot_log("TELEMETRY TASK STARTED\r\n");

    DSP_Output_t dsp;
    char msg[128];

    for(;;)
    {
        if(osMessageQueueGet(
                dspQueueHandle,
                &dsp,
                NULL,
                osWaitForever) == osOK)
        {
            snprintf(
                msg,
                sizeof(msg),
                "{\"rms\":%.3f,\"freq\":%.2f,\"temp\":%.2f,\"health\":%d}\r\n",
                dsp.rms,
                dsp.dominant_freq,
                dsp.temperature,
                dsp.health);

            HAL_UART_Transmit(
                &huart3,
                (uint8_t*)msg,
                strlen(msg),
                100);
        }
    }
}

/* ===================================================== */
/* ================= OTA TASK =========================== */
/* ===================================================== */

void StartOtaTask(void *argument)
{
    boot_log("OTA TASK STARTED\r\n");

    for(;;)
    {
        if(check_update_available())
        {
            boot_log("OTA UPDATE FOUND\r\n");

            download_firmware();

            ota_mark_update(0);

            boot_log("OTA REBOOT\r\n");

            NVIC_SystemReset();
        }

        osDelay(60000);
    }
}
