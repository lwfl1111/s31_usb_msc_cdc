/* SPDX-License-Identifier: CC0-1.0 */
#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_utmi_ll.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_console.h"
#include "vfs_tinyusb.h"
#include "tusb.h"
#include "device/dcd.h"

static TaskHandle_t s_app_task;
static uint32_t s_tx_dropped;

/* Mosaico workaround: stop HS DMA before a CPU restart reuses RAM. */
static void usb_shutdown(void)
{
    dcd_int_disable(0);
    usb_utmi_ll_reset_register();
    usb_utmi_ll_enable_bus_clock(false);
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;
    /* Never wait for TX in the USB task. The application task drains RX. */
    xTaskNotifyGive(s_app_task);
}

void app_main(void)
{
    s_app_task = xTaskGetCurrentTaskHandle();
    const tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb_config));
    ESP_ERROR_CHECK(esp_register_shutdown_handler(usb_shutdown));
    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_config));
    /* Only this task reads RX; do not add a competing stdin reader. */
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
    /* Preserve echoed CR/LF bytes; printf below supplies its own CRLF. */
    esp_vfs_tusb_cdc_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    ESP_LOGI("usb_cdc", "CDC console ready: printf and ESP_LOG now use USB");

    uint8_t rx[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    TickType_t last_status = xTaskGetTickCount();
    uint32_t status_count = 0;
    while (true) {
        /* Poll too, including RX received during initialization. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        size_t received = 0;
        ESP_ERROR_CHECK(tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rx,
                                           sizeof(rx), &received));
        if (received > 0 && tud_cdc_n_connected(0)) {
            /* Use stdout's serialization, shared with printf and ESP_LOG. */
            clearerr(stdout);
            size_t written = fwrite(rx, 1, received, stdout);
            s_tx_dropped += received - written;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - last_status >= pdMS_TO_TICKS(1000)) {
            last_status = now;
            if (tud_cdc_n_connected(0)) {
                clearerr(stdout);
                printf("\r\n[printf] alive=%" PRIu32 " heap=%" PRIu32
                       " echo_tx_dropped=%" PRIu32 "\r\n",
                       ++status_count, esp_get_free_heap_size(), s_tx_dropped);
                ESP_LOGI("usb_cdc", "[ESP_LOGI] CDC console alive=%" PRIu32,
                         status_count);
            }
        }
    }
}
