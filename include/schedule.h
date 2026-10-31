/*
 * schedule.h -- MEMBER 4 implements everything declared here.
 * Delete src/stub_schedule.c when you add src/schedule.c.
 *
 * REQUIREMENTS
 *  - is_access_permitted(): true if the CURRENT local time is inside the configured
 *    open window. access_control.c already has a weak version; your strong definition
 *    replaces it automatically. It is called from the RFID task AND the monitor task,
 *    so take g_config_mutex inside it (short critical section, no blocking calls).
 *    Handle windows that cross midnight. If the clock was never set, fail OPEN.
 *  - schedule_set(): caller (CLI) has ALREADY authenticated the admin. Validate the
 *    values, take g_config_mutex, store them, persist if you want, and call
 *    system_log_post(SYSLOG_CONFIG_CHANGE, ...).
 *  - Use localtime_r(); TZ is already EAT.
 */
#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t open_hour;    /* 0-23 */
    uint8_t open_min;     /* 0-59 */
    uint8_t close_hour;
    uint8_t close_min;
} schedule_cfg_t;

esp_err_t schedule_init(void);
bool      is_access_permitted(void);
esp_err_t schedule_get(schedule_cfg_t *out);
esp_err_t schedule_set(const schedule_cfg_t *cfg);

#endif