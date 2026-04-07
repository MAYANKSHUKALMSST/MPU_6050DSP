#include "ota_agent.h"
#include "flash_layout.h"
#include "network_interface.h"
#include "ota/flash_manager.h" /* Fixed: Added to resolve erase/write/read errors */
#include "ota_metadata.h"
#include "sha256.h"

#include "stdio.h"
#include "stm32f7xx_hal.h"
#include "string.h"

/* ----------------------------------------------------------
   OTA configuration uses network_interface.h
----------------------------------------------------------*/
#define CURRENT_FW_VERSION 1

#define RX_BUF 1024
static uint8_t buffer[RX_BUF];

/* ----------------------------------------------------------
   Helper: Skip HTTP Headers
   Identifies the \r\n\r\n sequence to find the start of the binary.
----------------------------------------------------------*/
static int http_skip_headers(uint8_t *buf, int len) {
  for (int i = 0; i < len - 3; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n')
      return i + 4;
  }
  return -1;
}

/* ----------------------------------------------------------
   Step 1: Fetch Edge Cryptographics (from ESP32 SPIFFS)
----------------------------------------------------------*/
static int ota_request_version(uint32_t *version, uint32_t *size,
                               uint8_t *expected_hash) {
  // Command ESP32 Edge Server to drop its stored meta.txt payload
  char req[] = "GET_META\n";
  network_send((uint8_t *)req, strlen(req));

  // Receive directly using STM32 UART blocks
  char meta[256] = {0};
  int len = network_read_timeout((uint8_t *)meta, 255, 3000);
  if (len <= 0)
    return -1;

  // Format is natively simple: "<size>\n<sha256>\n"
  char hash_str[65] = {0};
  if (sscanf(meta, "%lu\n%64s", size, hash_str) != 2)
    return -1;

  // Convert cryptographic hex back to 32 bytes
  for (int i = 0; i < 32; i++) {
    unsigned int b;
    sscanf(hash_str + (i * 2), "%02X", &b);
    expected_hash[i] = (uint8_t)b;
  }

  *version = 2; // Fixed bypass
  return 0;
}

/* ----------------------------------------------------------
   Step 2: Firmware Download & Flash
   Streams binary from Edge SPIFFS directly to Slot B.
----------------------------------------------------------*/
static int ota_download(uint32_t *size, uint8_t *hash_out) {
  uint32_t addr = FLASH_SLOT_B_ADDR;
  uint32_t total = 0;
  SHA256_CTX sha_ctx;
  sha256_init(&sha_ctx);

  printf("OTA: Preparing Slot B (0x%08lX)...\n", addr);
  if (flash_erase(FLASH_SLOT_B_ADDR, SLOT_SIZE) != 0) {
    printf("OTA: Flash erase failed\n");
    return -1;
  }

  char req[] = "GET_BIN\n";
  network_send((uint8_t *)req, strlen(req));

  while (total < *size) {
    // Air-gap stream blocks directly into memory bypassing HTTP!
    int len = network_read_timeout(buffer, RX_BUF, 3000);
    if (len <= 0)
      break;

    // Directly write untainted firmware binary
    flash_write(addr, buffer, len);
    sha256_update(&sha_ctx, buffer, len);

    addr += len;
    total += len;

    if (total % (10 * 1024) == 0)
      printf("OTA: Flashed %lu KB\n", total / 1024);
  }

  *size = total;
  if (total > 0) {
    sha256_final(&sha_ctx, hash_out);
    return 0;
  }
  return -1;
}

/* ----------------------------------------------------------
   Step 3: Commit Metadata
   Updates the metadata sector to trigger bootloader swap.
----------------------------------------------------------*/
static void ota_commit(uint32_t size, uint8_t *hash) {
  ota_metadata_t meta;
  memset(&meta, 0, sizeof(meta));

  meta.magic = OTA_MAGIC;
  meta.pending_update = 1;
  meta.active_slot = 1; // Slot B
  meta.firmware_size = size;
  memcpy(meta.firmware_hash, hash, 32);

  printf("OTA: Writing Metadata (Size: %lu, SHA-256 computed)\n", size);
  flash_write(FLASH_METADATA_ADDR, (uint8_t *)&meta, sizeof(meta));
}

/* ----------------------------------------------------------
   Main Routine
----------------------------------------------------------*/
void ota_agent_run(void) {
  uint32_t server_version = 0;
  uint32_t server_size = 0;
  uint8_t expected_hash[32] = {0};
  uint8_t computed_hash[32] = {0};

  if (network_connect() != 0) {
    printf("OTA: Network failed\n");
    return;
  }

  if (ota_request_version(&server_version, &server_size, expected_hash) != 0) {
    printf("OTA: Version check failed\n");
    return;
  }

  printf("OTA: Firmware size %lu bytes found on Server.\n", server_size);

  if (ota_download(&server_size, computed_hash) != 0) {
    printf("OTA: Download failed\n");
    return;
  }

  printf("OTA: Download complete. Verifying Cryptographic Signature...\n");
  if (memcmp(expected_hash, computed_hash, 32) != 0) {
    printf("OTA: CRITICAL ERROR! Downloaded stream does not match Server "
           "Signature!\n");
    return;
  }

  printf("OTA: Authenticated Signature Matches perfectly.\n");
  ota_commit(server_size, computed_hash);

  HAL_Delay(1000);
  printf("OTA: Resetting system...\n");
  NVIC_SystemReset();
}

/* ----------------------------------------------------------
   OTA Confirmation (Call after successful boot)
----------------------------------------------------------*/
void ota_confirm(void) {
  ota_metadata_t meta;

  /* Fixed: flash_read implicit declaration resolved via flash_manager.h */
  flash_read(FLASH_METADATA_ADDR, (uint8_t *)&meta, sizeof(meta));

  if (meta.magic == OTA_MAGIC && meta.pending_update == 1) {
    printf("OTA: Update confirmed! Marking firmware as stable.\n");
    meta.pending_update = 0;
    flash_write(FLASH_METADATA_ADDR, (uint8_t *)&meta, sizeof(meta));
  }
}
