#include "uart_driver.h"
#include "main.h"
#include <string.h>

extern UART_HandleTypeDef huart2;


/* ----------------------------------------------------------
   Send string over UART
----------------------------------------------------------*/
void uart_send(const char *str)
{
    HAL_UART_Transmit(&huart2,
                      (uint8_t*)str,
                      strlen(str),
                      HAL_MAX_DELAY);
}


/* ----------------------------------------------------------
   Receive data from UART
----------------------------------------------------------*/
int uart_receive(uint8_t *buffer, uint16_t len)
{
    if(HAL_UART_Receive(&huart2,
                        buffer,
                        len,
                        1000) == HAL_OK)
    {
        return len;
    }

    return -1;
}
