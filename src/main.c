/*
 * main.c  --  MEMBER 5: integration. Starts every module once.
 * When Member 3's system_sync_init() is ready, it replaces the three
 * xTaskCreate lines below (it spawns the same tasks with queues/mutexes).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "access_control.h"    /* Member 1 */
#include "lighting_control.h"  /* Member 2 */
#include "data_storage.h"      /* Member 4 */
#include "schedule.h"          /* Member 4 */
#include "serial_cli.h"        /* Member 5 */

static const char *TAG = "MAIN";

void app_main(void)
{
    /* The ESP32 has no battery clock, so start at 09:00 (building open).
     * Change it any time with the CLI command: settime HH:MM */
    schedule_set_time(9, 0);

    /* Order matters: access control creates the mutex the CLI relies on. */
    ESP_ERROR_CHECK(access_control_init());
    lighting_control_init();

    /* The SD card is optional for the demo: warn but keep going. */
    esp_err_t sd = sd_init();
    if (sd != ESP_OK) ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(sd));

    ESP_ERROR_CHECK(serial_cli_init());

    xTaskCreate(vTaskAccessControl,   "access",   4096, NULL, 5, NULL);
    xTaskCreate(vTaskLightingControl, "lighting", 3072, NULL, 4, NULL);
    xTaskCreate(vTaskSerialCLI,       "cli",      4096, NULL, 3, NULL);
}