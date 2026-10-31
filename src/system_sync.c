/*
 * system_sync.c -- MEMBER 3: FreeRTOS primitives, task creation, system monitor
 */
#include "system_sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "storage_sync.h"
#include "schedule.h"
#include "serial_cli.h"

static const char *TAG = "SYS_SYNC";

/* ---- Task configuration (stack size in BYTES, higher number = higher priority) ---- */
#define STACK_ACCESS      4096
#define STACK_LIGHTING    3072
#define STACK_MONITOR     3072
#define STACK_STORAGE     8192
#define STACK_CLI         6144

#define PRIO_ACCESS       5
#define PRIO_MONITOR      4
#define PRIO_LIGHTING     3
#define PRIO_STORAGE      2
#define PRIO_CLI          2

#define ACCESS_QUEUE_LEN  16
#define SYSLOG_QUEUE_LEN  16

#define MONITOR_PERIOD_MS          50
#define INTRUSION_LOG_INTERVAL_MS  30000

/* ---- Shared handles ---- */
EventGroupHandle_t g_system_events = NULL;
SemaphoreHandle_t  g_spi_mutex     = NULL;
SemaphoreHandle_t  g_sd_mutex      = NULL;
SemaphoreHandle_t  g_config_mutex  = NULL;
QueueHandle_t      g_access_queue  = NULL;
QueueHandle_t      g_syslog_queue  = NULL;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */
const char *system_log_type_str(system_log_type_t t)
{
    switch (t) {
    case SYSLOG_INFO:             return "INFO";
    case SYSLOG_EVACUATION_START: return "EVACUATION_START";
    case SYSLOG_EVACUATION_END:   return "EVACUATION_END";
    case SYSLOG_INTRUSION:        return "INTRUSION_ALERT";
    case SYSLOG_CONFIG_CHANGE:    return "CONFIG_CHANGE";
    default:                      return "UNKNOWN";
    }
}

bool system_log_post(system_log_type_t type, const char *fmt, ...)
{
    if (!g_syslog_queue) return false;

    system_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp = time(NULL);
    ev.type      = type;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "%s: %s", system_log_type_str(type), ev.text);

    /* Non-blocking: never stall the caller because the logger is slow */
    if (xQueueSend(g_syslog_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "syslog queue full, event dropped");
        return false;
    }
    return true;
}

bool system_evacuation_active(void)
{
    if (!g_system_events) return false;
    return (xEventGroupGetBits(g_system_events) & SYS_EVT_EVACUATION) != 0;
}

void system_evacuation_set(bool active, const char *source)
{
    if (!g_system_events) return;
    if (system_evacuation_active() == active) return;   /* no change */

    if (active) xEventGroupSetBits(g_system_events, SYS_EVT_EVACUATION);
    else        xEventGroupClearBits(g_system_events, SYS_EVT_EVACUATION);

    system_log_post(active ? SYSLOG_EVACUATION_START : SYSLOG_EVACUATION_END,
                    "Evacuation %s by %s",
                    active ? "ACTIVATED" : "CLEARED",
                    source ? source : "unknown");
}

/* Boot clock: EAT timezone; if the clock was never set, start at a fixed daytime
 * so the schedule is "open" at boot. The CLI can change it later. */
static void clock_init_default(void)
{
    setenv("TZ", "EAT-3", 1);
    tzset();

    if (time(NULL) < 1700000000) {   /* clock still at 1970 -> not set */
        struct tm t = {0};
        t.tm_year = 2026 - 1900;
        t.tm_mon  = 10;              /* November (0-based) */
        t.tm_mday = 2;
        t.tm_hour = 9;
        t.tm_isdst = 0;
        struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock not set: default 2026-11-02 09:00 EAT");
    }
}

/* ================================================================== */
/*  Init                                                               */
/* ================================================================== */
esp_err_t system_sync_init(void)
{
    if (g_system_events) return ESP_OK;   /* already initialised */

    g_system_events = xEventGroupCreate();
    g_spi_mutex     = xSemaphoreCreateMutex();
    g_sd_mutex      = xSemaphoreCreateMutex();
    g_config_mutex  = xSemaphoreCreateMutex();
    g_access_queue  = xQueueCreate(ACCESS_QUEUE_LEN, sizeof(access_event_t));
    g_syslog_queue  = xQueueCreate(SYSLOG_QUEUE_LEN, sizeof(system_log_event_t));

    if (!g_system_events || !g_spi_mutex || !g_sd_mutex || !g_config_mutex ||
        !g_access_queue || !g_syslog_queue) {
        ESP_LOGE(TAG, "failed to allocate sync primitives");
        return ESP_ERR_NO_MEM;
    }

    /* Hand the shared objects to Member 1's module */
    access_control_set_spi_mutex(g_spi_mutex);
    access_control_set_event_queue(g_access_queue);

    /* Evacuation pushbutton: active-low with internal pull-up */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << EVAC_BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    clock_init_default();

    ESP_LOGI(TAG, "sync primitives ready (3 mutexes, 2 queues, 1 event group)");
    return ESP_OK;
}

/* ================================================================== */
/*  System monitor task: evacuation button + evacuation fan-out +      */
/*  intrusion detection                                                */
/* ================================================================== */
static void check_intrusion(bool seen[3], TickType_t last[3])
{
    bool intruder = false;

    if (!is_access_permitted()) {                   /* building is CLOSED */
        for (int room = 1; room <= 2; room++) {
            if (!lighting_get_motion(room)) continue;
            intruder = true;
            TickType_t now = xTaskGetTickCount();
            if (!seen[room] ||
                (now - last[room]) >= pdMS_TO_TICKS(INTRUSION_LOG_INTERVAL_MS)) {
                seen[room] = true;
                last[room] = now;
                schedule_check_intrusion(true);   /* Member 4's alert; it posts SYSLOG_INTRUSION */
            }
        }
    }

    if (intruder) xEventGroupSetBits(g_system_events, SYS_EVT_INTRUSION);
    else          xEventGroupClearBits(g_system_events, SYS_EVT_INTRUSION);
}

static void vTaskSystemMonitor(void *pvParameters)
{
    (void)pvParameters;
    bool       evac_applied = false;
    int        btn_prev     = 1;
    bool       seen[3]      = { false, false, false };
    TickType_t last[3]      = { 0, 0, 0 };

    ESP_LOGI(TAG, "system monitor running");

    for (;;) {
        /* 1. Pushbutton: toggle evacuation on a falling edge */
        int btn = gpio_get_level(EVAC_BUTTON_PIN);
        if (btn == 0 && btn_prev == 1) {
            system_evacuation_set(!system_evacuation_active(), "button");
        }
        btn_prev = btn;

        /* 2. Apply evacuation to every module when the event bit changes */
        bool evac = system_evacuation_active();
        if (evac != evac_applied) {
            access_control_set_evacuation(evac);
            lighting_set_evacuation(evac);
            evac_applied = evac;
        }

        /* 3. Intrusion detection */
        check_intrusion(seen, last);

        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

/* ================================================================== */
/*  Task creation                                                      */
/* ================================================================== */
static bool spawn(TaskFunction_t fn, const char *name, uint32_t stack, UBaseType_t prio)
{
    BaseType_t r = xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, NULL, TASK_CORE);
    if (r != pdPASS) ESP_LOGE(TAG, "failed to create task '%s'", name);
    return r == pdPASS;
}

esp_err_t system_sync_start_tasks(void)
{
    bool ok = true;
    ok &= spawn(vTaskAccessControl,   "access",   STACK_ACCESS,   PRIO_ACCESS);
    ok &= spawn(vTaskSystemMonitor,   "monitor",  STACK_MONITOR,  PRIO_MONITOR);
    ok &= spawn(vTaskLightingControl, "lighting", STACK_LIGHTING, PRIO_LIGHTING);
    ok &= spawn(vTaskStorage,         "storage",  STACK_STORAGE,  PRIO_STORAGE);
    ok &= spawn(vTaskCLI,             "cli",      STACK_CLI,      PRIO_CLI);

    if (!ok) return ESP_FAIL;
    ESP_LOGI(TAG, "all tasks started");
    return ESP_OK;
}