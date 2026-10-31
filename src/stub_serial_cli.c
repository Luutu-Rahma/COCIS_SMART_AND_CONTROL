#include "serial_cli.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "CLI_STUB";

esp_err_t serial_cli_init(void)
{
    return ESP_OK;
}

void vTaskCLI(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGW(TAG, "STUB: CLI not implemented yet");
    vTaskDelete(NULL);
}