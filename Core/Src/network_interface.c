#include "network_interface.h"
#include "main.h"
#include "uart_driver.h"


#include <stdio.h>
#include <string.h>


extern UART_HandleTypeDef huart2;

#define RX_BUFFER_SIZE 512
static uint8_t rx_buffer[RX_BUFFER_SIZE];

/* ----------------------------------------------------------
   Wait for Bridge Response
----------------------------------------------------------*/
static int wait_for_response(const char *resp, uint32_t timeout) {
  uint32_t start = HAL_GetTick();
  uint16_t idx = 0;

  memset(rx_buffer, 0, RX_BUFFER_SIZE);

  while (HAL_GetTick() - start < timeout) {
    uint8_t ch;
    if (HAL_UART_Receive(&huart2, &ch, 1, 5) != HAL_OK) {
      continue;
    }

    if (idx < RX_BUFFER_SIZE - 1) {
      rx_buffer[idx++] = ch;
      rx_buffer[idx] = 0;

      if (strstr((char *)rx_buffer, resp)) {
        return 0;
      }
    }
  }
  return -1;
}

/* ----------------------------------------------------------
   Network Connect (WiFi is handled fully by Arduino Bridge)
----------------------------------------------------------*/
int network_connect(void) {
  // Give the Arduino bridge brief time to boot and hook WiFi
  osDelay(2500);
  return 0;
}

/* ----------------------------------------------------------
   TCP modular socket control
----------------------------------------------------------*/
int network_tcp_start(const char *ip, int port) {
  char cmd[64];
  sprintf(cmd, "TCP_START:%d\n", port);

  // We don't send IP because the Arduino Bridge knows to use the
  // defaultServer global variable to reach the Web Dashboard PC.
  HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), HAL_MAX_DELAY);

  if (wait_for_response("OK", 5000) != 0)
    return -1;

  return 0;
}

int network_tcp_stop(void) {
  char cmd[] = "TCP_STOP\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), HAL_MAX_DELAY);
  wait_for_response("OK", 1000);
  return 0;
}

/* ----------------------------------------------------------
   Transparent Protocol Transmission
----------------------------------------------------------*/
int network_send(uint8_t *data, uint32_t len) {
  HAL_UART_Transmit(&huart2, data, len, HAL_MAX_DELAY);
  return len;
}

int network_read(uint8_t *buffer, uint32_t len) {
  return network_read_timeout(buffer, len, 2000);
}

int network_read_timeout(uint8_t *buffer, uint32_t len, uint32_t timeout) {
  if (HAL_UART_Receive(&huart2, buffer, len, timeout) == HAL_OK) {
    return len;
  }
  return -1;
}
