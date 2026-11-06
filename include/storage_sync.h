#ifndef STORAGE_SYNC_H
#define STORAGE_SYNC_H

#include "esp_err.h"

/* mount the SD card after access_control_init; sets SYS_EVT_SD_READY on success */
esp_err_t storage_init(void);

/* sole consumer of g_access_queue and g_syslog_queue */
void vTaskStorage(void *pvParameters);

/* print the log while holding the storage locks */
void storage_print_log(void);

#endif

