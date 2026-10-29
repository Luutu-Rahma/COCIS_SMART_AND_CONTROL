/*
 * LOCAL TEST ONLY - paste into src/main.c on your own machine to test
 * your module alone. DO NOT commit this: main.c is shared, and Member 3's
 * system_sync_init() will start your task properly later.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "access_control.h"

void app_main(void)
{
    ESP_ERROR_CHECK(access_control_init());
    xTaskCreate(vTaskAccessControl, "access", 4096, NULL, 5, NULL);
}