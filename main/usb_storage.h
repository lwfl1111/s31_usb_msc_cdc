#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t usb_storage_init(void);
uint32_t usb_storage_capacity_bytes(void);
uint32_t usb_storage_flash_bytes(void);
