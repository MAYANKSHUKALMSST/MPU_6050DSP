#ifndef MPU6050_H
#define MPU6050_H

#include "stm32f7xx_hal.h"

#define MPU6050_I2C_ADDR          (0x68 << 1)
#define MPU6050_I2C_TIMEOUT       100

#define MPU6050_REG_SMPLRT_DIV    0x19
#define MPU6050_REG_CONFIG        0x1A
#define MPU6050_REG_GYRO_CONFIG   0x1B
#define MPU6050_REG_ACCEL_CONFIG  0x1C
#define MPU6050_REG_INT_ENABLE    0x38
#define MPU6050_REG_ACCEL_XOUT_H  0x3B
#define MPU6050_REG_TEMP_OUT_H    0x41
#define MPU6050_REG_GYRO_XOUT_H   0x43
#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_WHO_AM_I      0x75

#define ACCEL_SCALE_2G   16384.0f
#define GYRO_SCALE_250   131.0f

typedef struct {
    float accel_x, accel_y, accel_z;
    float gyro_x,  gyro_y,  gyro_z;
    float temp_c;
} MPU6050_Data_t;

HAL_StatusTypeDef MPU6050_Init   (I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef MPU6050_ReadAll(I2C_HandleTypeDef *hi2c, MPU6050_Data_t *data);
uint8_t           MPU6050_WhoAmI (I2C_HandleTypeDef *hi2c);

#endif
