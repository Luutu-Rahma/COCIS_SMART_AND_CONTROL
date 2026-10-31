#include "schedule.h"
#include "esp_log.h"

static const char *TAG = "SCHEDULE_STUB";

esp_err_t schedule_init(void)
{
    ESP_LOGW(TAG, "STUB: schedule not implemented (access always permitted)");
    return ESP_OK;
}

esp_err_t schedule_get(schedule_cfg_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    out->open_hour = 7;  out->open_min = 0;
    out->close_hour = 22; out->close_min = 0;
    return ESP_OK;
}

esp_err_t schedule_set(const schedule_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

/* NOTE: is_access_permitted() is NOT defined here on purpose: access_control.c
 * already has a default (always true) until Member 4 provides the real one. */