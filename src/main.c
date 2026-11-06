/* initialize access control before storage; both share the SPI bus */
#include "esp_err.h"
#include "esp_log.h"
#include "system_sync.h"
#include "access_control.h"
#include "lighting_control.h"
#include "storage_sync.h"
#include "serial_cli.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_ERROR_CHECK(system_sync_init());//sync objs

    ESP_ERROR_CHECK(access_control_init());
    
    lighting_control_init();

    esp_err_t err = storage_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "storage_init: %s (continuing without SD)", esp_err_to_name(err));

    err = serial_cli_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "serial_cli_init: %s", esp_err_to_name(err));

    ESP_ERROR_CHECK(system_sync_start_tasks());//tasks start last
    ESP_LOGI(TAG, "system running");
}
