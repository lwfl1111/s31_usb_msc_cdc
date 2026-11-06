idf_component_register(SRCS "hello_world_main.c" "usb_storage.c"
                       PRIV_REQUIRES esp_system esp_hal_usb spi_flash esp_partition wear_levelling fatfs
                       INCLUDE_DIRS "")
