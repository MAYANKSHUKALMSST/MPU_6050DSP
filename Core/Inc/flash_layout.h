#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#define FLASH_BOOT_START      0x08000000
#define FLASH_BOOT_SIZE       (128 * 1024)

#define FLASH_SLOT_A_ADDR     0x08020000
#define FLASH_SLOT_B_ADDR     0x08060000

#define FLASH_METADATA_ADDR   0x080A0000

#define SLOT_SIZE             (256 * 1024)

#endif
