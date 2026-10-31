/*
 * main.c -- application entry point (Member 3)
 *
 * Init order matters:
 *   1. system_sync_init   : mutexes/queues/event group + default clock
 *   2. access_control_init: creates the SPI3 bus (SD card reuses it)
 *   3. lighting_control_init
 *   4. data_storage_init  : MUST come after access_control_init (needs the SPI bus)
 *   5. schedule_init, serial_cli_init
 *   6. system_sync_start_tasks: only now start the tasks
 */
#include "esp_err.h"
#include "esp_log.h"
#include "system_sync.h"
#include "access_control.h"
#include "lighting_control.h"
#include "data_storage.h"
#include "schedule.h"
#include "serial_cli.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_ERROR_CHECK(system_sync_init());
    ESP_ERROR_CHECK(access_control_init());
    lighting_control_init();

    esp_err_t err = data_storage_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "data_storage_init: %s (continuing without SD)", esp_err_to_name(err));

    err = schedule_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "schedule_init: %s", esp_err_to_name(err));

    err = serial_cli_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "serial_cli_init: %s", esp_err_to_name(err));

    ESP_ERROR_CHECK(system_sync_start_tasks());
    ESP_LOGI(TAG, "system running");
}