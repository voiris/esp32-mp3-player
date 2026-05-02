#include <dirent.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_types.h"
#include "driver/sdspi_host.h"

void mount_sd_card(void) {
    // Pullup
    gpio_set_pull_mode(CONFIG_MISO_GPIO_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_CS_GPIO_PIN, GPIO_PULLUP_ONLY);

    // SPI host
    sdmmc_host_t sdmmc_host = SDSPI_HOST_DEFAULT();
    sdmmc_host.slot = SPI2_HOST;
    sdmmc_host.max_freq_khz = CONFIG_MAX_BUS_FREQUENCY;

    // SPI bus configuration
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_MOSI_GPIO_PIN,
        .miso_io_num = CONFIG_MISO_GPIO_PIN,
        .sclk_io_num = CONFIG_SCK_GPIO_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CONFIG_MAX_TRANSFER_SIZE,
    };
    esp_err_t ret = spi_bus_initialize(sdmmc_host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        printf("SPI bus initialize failed: %s\n", esp_err_to_name(ret));
        return;
    }

    // SPI device configuration for SD card
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_CS_GPIO_PIN;
    slot_config.host_id = sdmmc_host.slot;
    slot_config.wait_for_miso = -1;

    // Mount filesystem
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = CONFIG_MAX_OPENED_FILES,
        .allocation_unit_size = CONFIG_ALLOCATION_UNIT_SIZE,
    };

    sdmmc_card_t* card;
    ret = esp_vfs_fat_sdspi_mount(CONFIG_MOUNT_POINT, &sdmmc_host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        printf("mount failed: %s\n", esp_err_to_name(ret));
        return;
    }
}