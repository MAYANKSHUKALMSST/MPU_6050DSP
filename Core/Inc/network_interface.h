#ifndef NETWORK_INTERFACE_H
#define NETWORK_INTERFACE_H

#include <stdint.h>

#define SERVER_IP "161.118.167.196"
#define OTA_PORT 3000
#define TELEMETRY_PORT 3001

int network_connect(void);
int network_tcp_start(const char *ip, int port);
int network_tcp_stop(void);
int network_send(uint8_t *data, uint32_t len);
int network_read(uint8_t *buffer, uint32_t len);
int network_read_timeout(uint8_t *buffer, uint32_t len, uint32_t timeout);

#endif
