#include <stdio.h>
#include <time.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "data_storage.h"
#include "driver/gpio.h"

#define MOUNT_POINT "/sdcard"
#define LOG_FILE    MOUNT_POINT "/access_log.txt"

#define PIN_MOSI  23
#define PIN_MISO  19
#define PIN_SCK   18
#define PIN_CS_SD 2      // SD chip select; SPI data and clock pins are shared with RFID.

static sdmmc_card_t *card;
static void park_extra_chip_selects(void)
{
    const int pins[] = {0, 12};
    for (int i = 0; i < 2; i++) {
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 1);
    }
}

// Initialize the SD card on the shared SPI bus.
esp_err_t sd_init(void)
{   
    park_extra_chip_selects();
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;   // Shared with the RC522 readers.

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t r = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    // The access-control module may have initialized the bus already.
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs  = PIN_CS_SD;
    slot.host_id  = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &cfg, &card);
}

// Append a timestamped event to the log.
esp_err_t log_event_to_sd(const char *type, const char *uid)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &t);

    FILE *f = fopen(LOG_FILE, "a");
    if (f == NULL) return ESP_FAIL;
    fprintf(f, "%s,%s,%s\n", stamp, type, uid);
    fclose(f);
    return ESP_OK;
}

// Print the SD log.
void sd_print_log(void)
{
    FILE *f = fopen(LOG_FILE, "r");
    if (f == NULL) {
        printf("No log file yet\n");
        return;
    }
    char line[100];
    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }
    fclose(f);
}
