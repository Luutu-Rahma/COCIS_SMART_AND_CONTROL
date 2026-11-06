#pragma once
#include "esp_err.h"

esp_err_t sd_init(void);                                    // mount the SD card
esp_err_t log_event_to_sd(const char *type, const char *uid); // append a log entry
void sd_print_log(void);                                    // print the log
