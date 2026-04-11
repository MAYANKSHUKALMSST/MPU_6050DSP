#include "mpu6050.h"

/* Runtime I2C address — defaults to AD0 LOW (0xD0).
 * StartSensorTask probes both addresses and sets this before calling
 * MPU6050_Init, so the driver always uses the correct address.          */
uint16_t mpu6050_i2c_addr = MPU6050_ADDR_AD0_LOW;

/* ───────────────────────────────────────────────────────────────────────── */
/*                      LOW LEVEL INTERNAL FUNCTIONS                         */
/* ───────────────────────────────────────────────────────────────────────── */

/* Write one byte to register */
static HAL_StatusTypeDef mpu_write(I2C_HandleTypeDef *hi2c,
                                   uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return HAL_I2C_Master_Transmit(hi2c,
                                   MPU6050_I2C_ADDR,
                                   buf, 2,
                                   MPU6050_I2C_TIMEOUT);
}

/* Read N bytes starting from register */
static HAL_StatusTypeDef mpu_read(I2C_HandleTypeDef *hi2c,
                                  uint8_t reg,
                                  uint8_t *buf,
                                  uint16_t len)
{
    HAL_StatusTypeDef s;

    s = HAL_I2C_Master_Transmit(hi2c,
                                MPU6050_I2C_ADDR,
                                &reg, 1,
                                MPU6050_I2C_TIMEOUT);
    if (s != HAL_OK)
        return s;

    return HAL_I2C_Master_Receive(hi2c,
                                  MPU6050_I2C_ADDR,
                                  buf, len,
                                  MPU6050_I2C_TIMEOUT);
}

/* ───────────────────────────────────────────────────────────────────────── */
/*                          PUBLIC API FUNCTIONS                             */
/* ───────────────────────────────────────────────────────────────────────── */

/* WHO_AM_I (should return 0x68) */
uint8_t MPU6050_WhoAmI(I2C_HandleTypeDef *hi2c)
{
    uint8_t id = 0;
    mpu_read(hi2c, MPU6050_REG_WHO_AM_I, &id, 1);
    return id;
}

/* Initialize sensor */
HAL_StatusTypeDef MPU6050_Init(I2C_HandleTypeDef *hi2c)
{
    HAL_StatusTypeDef s;

    /* Wake up */
    s = mpu_write(hi2c, MPU6050_REG_PWR_MGMT_1, 0x00);
    if (s != HAL_OK) return s;
    HAL_Delay(100);

    /* Sample rate: 500 Hz  (Gyro output rate 1 kHz, SMPLRT_DIV=1 → 1000/2) */
    s = mpu_write(hi2c, MPU6050_REG_SMPLRT_DIV, 0x01);
    if (s != HAL_OK) return s;

    /* DLPF: 184 Hz bandwidth — anti-aliasing for 500 Hz sampling (Nyquist 250 Hz) */
    s = mpu_write(hi2c, MPU6050_REG_CONFIG, 0x01);
    if (s != HAL_OK) return s;

    /* Gyro ±250 dps */
    s = mpu_write(hi2c, MPU6050_REG_GYRO_CONFIG, 0x00);
    if (s != HAL_OK) return s;

    /* Accel ±2g */
    /* Enable Data Ready interrupt */
    s = mpu_write(hi2c, MPU6050_REG_INT_ENABLE, 0x01);

    return s;
}

/* ───────────────────────────────────────────────────────────────────────── */
/*                       HIGH PERFORMANCE BURST READ                         */
/* ───────────────────────────────────────────────────────────────────────── */

HAL_StatusTypeDef MPU6050_ReadAll(I2C_HandleTypeDef *hi2c,
                                  MPU6050_Data_t *data)
{
    uint8_t raw[14];

    HAL_StatusTypeDef s =
        mpu_read(hi2c, MPU6050_REG_ACCEL_XOUT_H, raw, 14);

    if (s != HAL_OK)
        return s;

    int16_t ax  = (int16_t)((raw[0]  << 8) | raw[1]);
    int16_t ay  = (int16_t)((raw[2]  << 8) | raw[3]);
    int16_t az  = (int16_t)((raw[4]  << 8) | raw[5]);
    int16_t tmp = (int16_t)((raw[6]  << 8) | raw[7]);
    int16_t gx  = (int16_t)((raw[8]  << 8) | raw[9]);
    int16_t gy  = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz  = (int16_t)((raw[12] << 8) | raw[13]);

    data->accel_x = (float)ax / ACCEL_SCALE_2G;
    data->accel_y = (float)ay / ACCEL_SCALE_2G;
    data->accel_z = (float)az / ACCEL_SCALE_2G;

    data->gyro_x  = (float)gx / GYRO_SCALE_250;
    data->gyro_y  = (float)gy / GYRO_SCALE_250;
    data->gyro_z  = (float)gz / GYRO_SCALE_250;

    data->temp_c  = (float)tmp / 340.0f + 36.53f;

    return HAL_OK;
}

/* ───────────────────────────────────────────────────────────────────────── */
/*                     MODULAR WRAPPER FUNCTIONS                             */
/* ───────────────────────────────────────────────────────────────────────── */

HAL_StatusTypeDef MPU6050_ReadAccel(I2C_HandleTypeDef *hi2c,
                                    MPU6050_Data_t *data)
{
    uint8_t raw[6];

    HAL_StatusTypeDef s =
        mpu_read(hi2c, MPU6050_REG_ACCEL_XOUT_H, raw, 6);

    if (s != HAL_OK)
        return s;

    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);

    data->accel_x = (float)ax / ACCEL_SCALE_2G;
    data->accel_y = (float)ay / ACCEL_SCALE_2G;
    data->accel_z = (float)az / ACCEL_SCALE_2G;

    return HAL_OK;
}

HAL_StatusTypeDef MPU6050_ReadTemp(I2C_HandleTypeDef *hi2c,
                                   MPU6050_Data_t *data)
{
    uint8_t raw[2];

    HAL_StatusTypeDef s =
        mpu_read(hi2c, MPU6050_REG_TEMP_OUT_H, raw, 2);

    if (s != HAL_OK)
        return s;

    int16_t tmp = (int16_t)((raw[0] << 8) | raw[1]);

    data->temp_c = (float)tmp / 340.0f + 36.53f;

    return HAL_OK;
}
