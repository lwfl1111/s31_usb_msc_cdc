/* SPDX-License-Identifier: CC0-1.0 */
#include "usb_storage.h"
#include "esp_check.h"
#include "esp_log.h"
#include "diskio_impl.h"
#include "diskio_wl.h"
#include <stdlib.h>
#include "esp_flash.h"
#include "esp_partition.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

static const char *TAG = "usb_storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_storage;
static uint32_t s_capacity;
static uint32_t s_flash_size;

/* Run before USB starts. Only completely erased logical media may be formatted. */
static esp_err_t prepare_fat(void)
{
    BYTE drive;
    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(&drive), TAG, "No free FAT drive");
    ESP_RETURN_ON_ERROR(ff_diskio_register_wl_partition(drive, s_wl), TAG,
                        "FAT disk registration failed");
    char path[] = {(char)('0' + drive), ':', 0};
    FATFS *fs = calloc(1, sizeof(*fs));
    uint8_t *buffer = malloc(CONFIG_WL_SECTOR_SIZE);
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (fs == NULL || buffer == NULL) {
        goto cleanup;
    }
    FRESULT result = f_mount(fs, path, 1);
    if (result == FR_OK) {
        ESP_LOGI(TAG, "Existing FAT filesystem preserved");
        ret = ESP_OK;
        goto cleanup;
    }
    if (result != FR_NO_FILESYSTEM) {
        ESP_LOGE(TAG, "FAT mount failed (%d); refusing to format", result);
        ret = ESP_FAIL;
        goto cleanup;
    }
    /* A damaged/nonblank disk is not a new disk. Never erase it automatically. */
    for (size_t offset = 0; offset < wl_size(s_wl); offset += CONFIG_WL_SECTOR_SIZE) {
        ret = wl_read(s_wl, offset, buffer, CONFIG_WL_SECTOR_SIZE);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        for (size_t i = 0; i < CONFIG_WL_SECTOR_SIZE; ++i) {
            if (buffer[i] != 0xff) {
                ESP_LOGE(TAG, "Nonblank media has no valid FAT; refusing to erase existing data");
                ret = ESP_ERR_INVALID_STATE;
                goto cleanup;
            }
        }
    }
    ESP_LOGI(TAG, "Creating FAT filesystem on blank MSC media");
    const MKFS_PARM options = {
        .fmt = FM_FAT | FM_SFD,
        .n_fat = 2,
        .au_size = CONFIG_WL_SECTOR_SIZE,
    };
    result = f_mkfs(path, &options, buffer, CONFIG_WL_SECTOR_SIZE);
    if (result == FR_OK) {
        result = f_mount(fs, path, 1);
    }
    ret = result == FR_OK ? ESP_OK : ESP_FAIL;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAT initialization failed (%d)", result);
    }
cleanup:
    (void)f_mount(NULL, path, 0);
    ff_diskio_clear_pdrv_wl(s_wl);
    ff_diskio_unregister(drive);
    free(buffer);
    free(fs);
    return ret;
}

esp_err_t usb_storage_init(void)
{
    /* Keep an empty MSC driver alive on media errors: callbacks require it. */
    const tinyusb_msc_driver_config_t driver_config = {
        .user_flags.auto_mount_off = 1,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_config), TAG,
                        "MSC driver installation failed");
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "msc_fat");
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "Missing msc_fat: flash the new partition table too");
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &s_flash_size), TAG, "Flash size query failed");
    ESP_RETURN_ON_FALSE(partition->address == 0x110000 && partition->size == 0xef0000,
                        ESP_ERR_INVALID_SIZE, TAG, "Unexpected MSC partition layout");
    ESP_RETURN_ON_FALSE(s_flash_size >= 0x1000000, ESP_ERR_INVALID_SIZE, TAG,
                        "This layout requires at least 16 MiB NOR flash");
    ESP_RETURN_ON_ERROR(wl_mount(partition, &s_wl), TAG, "Wear levelling initialization failed");

    esp_err_t ret = prepare_fat();
    if (ret != ESP_OK) {
        (void)wl_unmount(s_wl);
        s_wl = WL_INVALID_HANDLE;
        return ret;
    }
    /* FAT is now unmounted locally; USB exclusively owns storage from here. */
    const tinyusb_msc_storage_config_t storage_config = {
        .medium.wl_handle = s_wl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs.do_not_format = true,
    };
    ret = tinyusb_msc_new_storage_spiflash(&storage_config, &s_storage);
    if (ret != ESP_OK) {
        (void)wl_unmount(s_wl);
        return ret;
    }
    uint32_t sectors;
    uint32_t sector_size;
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_capacity(s_storage, &sectors), TAG, "Capacity query failed");
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_sector_size(s_storage, &sector_size), TAG, "Sector query failed");
    s_capacity = sectors * sector_size;
    return ESP_OK;
}

uint32_t usb_storage_capacity_bytes(void)
{
    return s_capacity;
}

uint32_t usb_storage_flash_bytes(void)
{
    return s_flash_size;
}
