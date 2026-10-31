/*
 * storage_sync.c -- MEMBER 3: queue consumer + locking around Member 4's SD functions.
 * Lock order (see system_sync.h): g_sd_mutex -> g_spi_mutex.
 */
#include "storage_sync.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "system_sync.h"
#include "data_storage.h"

static const char *TAG = "STORAGE";
static volatile bool s_sd_ready = false;

static const char *gate_name(gate_t g)
{
    return (g == GATE_ENTRY) ? "ENTRY" : "EXIT";
}

static const char *ac_result_name(access_result_t r)
{
    switch (r) {
    case ACCESS_GRANTED:               return "GRANTED";
    case ACCESS_DENIED_UNKNOWN_TAG:    return "DENIED_UNKNOWN_TAG";
    case ACCESS_DENIED_SCHEDULE:       return "DENIED_SCHEDULE";
    case ACCESS_DENIED_ALREADY_INSIDE: return "DENIED_ALREADY_INSIDE";
    case ACCESS_DENIED_NOT_INSIDE:     return "DENIED_NOT_INSIDE";
    default:                           return "UNKNOWN";
    }
}

/* Write one line to the SD card with both locks held (SD first, then SPI). */
static esp_err_t locked_log(const char *type, const char *detail)
{
    if (!s_sd_ready) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    esp_err_t r = log_event_to_sd(type, detail);
    xSemaphoreGive(g_spi_mutex);
    xSemaphoreGive(g_sd_mutex);
    return r;
}

static void store(const char *type, const char *detail)
{
    ESP_LOGI(TAG, "[%s] %s", type, detail);
    if (!s_sd_ready) return;
    if (locked_log(type, detail) != ESP_OK) {
        ESP_LOGW(TAG, "SD write failed for event %s", type);
    }
}

esp_err_t storage_init(void)
{
    esp_err_t r = sd_init();
    if (r == ESP_OK) {
        s_sd_ready = true;
        xEventGroupSetBits(g_system_events, SYS_EVT_SD_READY);
        ESP_LOGI(TAG, "SD card mounted, logging to SD enabled");
    } else {
        ESP_LOGW(TAG, "SD mount failed (%s): events will only be printed to serial",
                 esp_err_to_name(r));
    }
    return r;
}

void storage_print_log(void)
{
    if (!s_sd_ready) {
        printf("SD card not available\n");
        return;
    }
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    sd_print_log();
    xSemaphoreGive(g_spi_mutex);
    xSemaphoreGive(g_sd_mutex);
}

void vTaskStorage(void *pvParameters)
{
    (void)pvParameters;
    access_event_t     ev;
    system_log_event_t sl;
    char type[40];
    char detail[112];

    for (;;) {
        if (xQueueReceive(g_access_queue, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            snprintf(type, sizeof(type), "%s_%s", gate_name(ev.gate), ac_result_name(ev.result));
            snprintf(detail, sizeof(detail), "%02X:%02X:%02X:%02X;%s;headcount=%d",
                     ev.uid[0], ev.uid[1], ev.uid[2], ev.uid[3],
                     ev.name[0] ? ev.name : "UNKNOWN", ev.headcount);
            store(type, detail);
        }

        while (xQueueReceive(g_syslog_queue, &sl, 0) == pdTRUE) {
            snprintf(detail, sizeof(detail), "%s", sl.text);
            for (char *p = detail; *p; p++) {          /* keep the CSV line intact */
                if (*p == ',' || *p == '\n' || *p == '\r') *p = ';';
            }
            store(system_log_type_str(sl.type), detail);
        }
    }
}

