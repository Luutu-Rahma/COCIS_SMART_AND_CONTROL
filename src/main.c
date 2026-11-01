/*
 * LOCAL TEST ONLY (do NOT commit): runs Member 1 (access) + Member 2 (lighting)
 * together. Member 3's system_sync_init() will replace this later.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "access_control.h"
#include "lighting_control.h"

void app_main(void)
{
    ESP_ERROR_CHECK(access_control_init());
    lighting_control_init();

    xTaskCreate(vTaskAccessControl,   "access",   4096, NULL, 5, NULL);
    xTaskCreate(vTaskLightingControl, "lighting", 4096, NULL, 4, NULL);
}