/*
 * data_storage.h -- MEMBER 4 implements everything declared here.
 * Delete src/stub_data_storage.c when you add src/data_storage.c.
 *
 * REQUIREMENTS
 *  - data_storage_init(): mount the SD card (FAT) on the EXISTING SPI3_HOST bus.
 *    access_control_init() has already called spi_bus_initialize(SPI3_HOST), so
 *    do NOT re-init the bus (tolerate ESP_ERR_INVALID_STATE if you do). SD CS = GPIO 2.
 *    On success set SYS_EVT_SD_READY in g_system_events. If the SD is missing, return
 *    an error; the system keeps running without it.
 *  - vTaskStorage(): the ONLY consumer of g_access_queue and g_syslog_queue. Loop:
 *    receive from g_access_queue (short timeout), then drain g_syslog_queue, and append
 *    one line per event to the log file with a timestamp. Each file write must hold
 *    g_config_mutex? NO -> take g_sd_mutex, then g_spi_mutex (lock order in system_sync.h).
 *  - data_storage_read_recent(): copy up to max_lines of the newest log lines into buf
 *    (NUL-terminated) for the CLI `logs` command. Same locking.
 */
#ifndef DATA_STORAGE_H
#define DATA_STORAGE_H

#include <stddef.h>
#include "esp_err.h"

esp_err_t data_storage_init(void);
void      vTaskStorage(void *pvParameters);
esp_err_t data_storage_read_recent(char *buf, size_t buf_len, int max_lines);

#endif