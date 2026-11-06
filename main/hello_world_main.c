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

static void cdc_send(const uint8_t *data, size_t length)
{
    const TickType_t start = xTaskGetTickCount();
    size_t sent = 0;
    while (sent < length && tud_cdc_n_connected(0)
           && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(100)) {
        sent += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                          data + sent, length - sent);
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
        if (sent < length) {
            vTaskDelay(1);
        }
    }
    s_tx_dropped += length - sent;
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
    ESP_LOGI("usb_cdc", "CDC ready: open the application COM port with DTR enabled");

    uint8_t rx[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    TickType_t last_status = xTaskGetTickCount();
    uint32_t status_count = 0;
    while (true) {
        /* Poll too, including RX received during initialization. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        size_t received = 0;
        ESP_ERROR_CHECK(tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rx,
                                           sizeof(rx), &received));
        if (received > 0) {
            cdc_send(rx, received);
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - last_status >= pdMS_TO_TICKS(1000)) {
            last_status = now;
            if (tud_cdc_n_connected(0)) {
                char status[160];
                int length = snprintf(status, sizeof(status),
                                      "\r\n[CDC] alive=%" PRIu32
                                      " heap=%" PRIu32 " tx_dropped=%" PRIu32 "\r\n",
                                      ++status_count, esp_get_free_heap_size(), s_tx_dropped);
                if (length > 0 && length < (int)sizeof(status)) {
                    cdc_send((const uint8_t *)status, (size_t)length);
                }
            }
        }
    }
}
