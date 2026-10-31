/*
 * serial_cli.h -- MEMBER 5 implements everything declared here.
 * Delete src/stub_serial_cli.c when you add src/serial_cli.c.
 *
 * Things your commands can call (all thread-safe):
 *   access_control_get_headcount(), access_control_list_inside(names, max)
 *   lighting_get_room_state(room), lighting_get_motion(room), lighting_get_brightness_pct(room)
 *   schedule_get(), schedule_set()  (only AFTER admin authentication)
 *   data_storage_read_recent(buf, len, max_lines)
 *   system_evacuation_set(true/false, "cli"), system_evacuation_active()
 *   settimeofday() for `set-time`; time(NULL)/localtime_r() to display
 *
 * Authentication is YOUR job: schedule_set and set-time must be refused unless the
 * admin password was entered correctly.
 */
#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include "esp_err.h"

esp_err_t serial_cli_init(void);
void      vTaskCLI(void *pvParameters);

#endif