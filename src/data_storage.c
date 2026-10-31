#include <stdio.h>
#include <time.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "data_storage.h"

#define MOUNT_POINT "/sdcard"
#define LOG_FILE    MOUNT_POINT "/access_log.txt"

#define PIN_MOSI  23
#define PIN_MISO  19
#define PIN_SCK   18
#define PIN_CS_SD 2      // my own chip select; the other pins are shared with the RFID readers

static sdmmc_card_t *card;

// Step 1: start the SPI bus and mount the SD card as a normal folder (/sdcard)
esp_err_t sd_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;   // Member 3: must be the same host as access_control (RC522_SPI_HOST) so RFID and SD share ONE bus

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t r = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    // INVALID_STATE just means another module already started the bus. That is fine.
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

// Step 2: add one line to the end of the log file: time, event type, card ID
esp_err_t log_event_to_sd(const char *type, const char *uid)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &t);

    FILE *f = fopen(LOG_FILE, "a");      // "a" = append, keeps old lines
    if (f == NULL) return ESP_FAIL;
    fprintf(f, "%s,%s,%s\n", stamp, type, uid);
    fclose(f);
    return ESP_OK;
}

// Step 3: read the file back and show it (proves the logging works)
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