/*
 * ota_agent.c
 *
 *  Created on: 05-Mar-2026
 *      Author: mayank.shukla
 */


#include "flash_layout.h"
#include "ota_metadata.h"

void ota_mark_update(uint32_t size)
{
	OTA_META->pending_update = 1;
    OTA_META->firmware_size = size;
}
