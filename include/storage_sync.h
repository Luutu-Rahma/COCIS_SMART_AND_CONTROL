/*
 * storage_sync.h -- MEMBER 3: thread-safe glue between the system queues and
 * Member 4's SD functions (data_storage.h). Member 4's data_storage.c still owns
 * the actual file I/O; this layer adds the locking and the queue consumer.
 */
#ifndef STORAGE_SYNC_H
#define STORAGE_SYNC_H

#include "esp_err.h"

/* Mount the SD card via Member 4's sd_init(); sets SYS_EVT_SD_READY on success.
 * Call AFTER access_control_init() (which creates the shared SPI3 bus). */
esp_err_t storage_init(void);

/* Task body. The ONLY consumer of g_access_queue and g_syslog_queue. */
void vTaskStorage(void *pvParameters);

/* Locked wrapper around sd_print_log() for the CLI `logs` command. */
void storage_print_log(void);

#endif

