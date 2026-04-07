#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdint.h>

void uart_send(const char *str);
int uart_receive(uint8_t *buffer, uint16_t len);

#endif
