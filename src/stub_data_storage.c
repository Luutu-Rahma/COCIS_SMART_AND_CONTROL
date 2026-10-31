#include "data_storage.h"
#include "system_sync.h"
#include "esp_log.h"

static const char *TAG = "STORAGE_STUB";

esp_err_t data_storage_init(void)
{
    ESP_LOGW(TAG, "STUB: no SD card yet (events are printed to serial only)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t data_storage_read_recent(char *buf, size_t buf_len, int max_lines)
{
    (void)max_lines;
    if (buf && buf_len) buf[0] = '\0';
    return ESP_ERR_NOT_SUPPORTED;
}

/* Reference loop: the real vTaskStorage must consume BOTH queues like this. */
void vTaskStorage(void *pvParameters)
{
    (void)pvParameters;
    access_event_t     ev;
    system_log_event_t sl;

    for (;;) {
        if (xQueueReceive(g_access_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "[access] gate=%d result=%d name=%s headcount=%d",
                     (int)ev.gate, (int)ev.result, ev.name, ev.headcount);
        }
        while (xQueueReceive(g_syslog_queue, &sl, 0) == pdTRUE) {
            ESP_LOGI(TAG, "[syslog] %s: %s", system_log_type_str(sl.type), sl.text);
        }
    }
}