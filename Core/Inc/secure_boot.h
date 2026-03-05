#ifndef SECURE_BOOT_H
#define SECURE_BOOT_H

#include <stdint.h>

int verify_firmware(uint32_t address,
                    uint32_t size,
                    uint8_t *expected_hash);

#endif
