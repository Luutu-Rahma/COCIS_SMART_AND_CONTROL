/* freertos synchronization and system monitor */
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

#define STACK_ACCESS      4096
#define STACK_LIGHTING    3072
#define STACK_MONITOR     4096   /* was 3072: evacuation reporting uses more stack */
#define STACK_STORAGE     8192
#define STACK_CLI         6144

#define PRIO_ACCESS       5
#define PRIO_MONITOR      4
#define PRIO_LIGHTING     3
#define PRIO_STORAGE      2
#define PRIO_CLI          2

#define ACCESS_QUEUE_LEN  16
#define SYSLOG_QUEUE_LEN  32   /* was 16: an evacuation roster posts a burst of lines */

#define MONITOR_PERIOD_MS          50
#define INTRUSION_LOG_INTERVAL_MS  30000
#define EVAC_STATUS_PERIOD_MS      10000   /* progress line while people are still inside */
#define EVAC_MAX_PEOPLE            16

EventGroupHandle_t g_system_events = NULL;
SemaphoreHandle_t  g_spi_mutex     = NULL;
SemaphoreHandle_t  g_sd_mutex      = NULL;
SemaphoreHandle_t  g_config_mutex  = NULL;
QueueHandle_t      g_access_queue  = NULL;
QueueHandle_t      g_syslog_queue  = NULL;

const char *system_log_type_str(system_log_type_t t)
{
    switch (t) {
    case SYSLOG_INFO:             return "INFO";
    case SYSLOG_EVACUATION_START: return "EVACUATION_START";
    case SYSLOG_EVACUATION_END:   return "EVACUATION_END";
    case SYSLOG_INTRUSION:        return "INTRUSION_ALERT";
    case SYSLOG_CONFIG_CHANGE:    return "CONFIG_CHANGE";
    case SYSLOG_AUTH_FAIL:        return "AUTH_FAIL";
    case SYSLOG_ADMIN_ACTION:     return "ADMIN_ACTION";
    case SYSLOG_EVAC_ROSTER:      return "EVAC_ROSTER";
    case SYSLOG_EVAC_MISSING:     return "EVAC_MISSING";
    case SYSLOG_EVAC_ALL_CLEAR:   return "EVAC_ALL_CLEAR";
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

    /* Keep logging non-blocking for callers. */
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
    if (system_evacuation_active() == active) return;

    if (active) xEventGroupSetBits(g_system_events, SYS_EVT_EVACUATION);
    else        xEventGroupClearBits(g_system_events, SYS_EVT_EVACUATION);

    system_log_post(active ? SYSLOG_EVACUATION_START : SYSLOG_EVACUATION_END,
                    "Evacuation %s by %s",
                    active ? "ACTIVATED" : "CLEARED",
                    source ? source : "unknown");
}

/* Set EAT and initialize an unset clock to daytime. */
static void clock_init_default(void)
{
    setenv("TZ", "EAT-3", 1);
    tzset();

    if (time(NULL) < 1700000000) {
        struct tm t = {0};
        t.tm_year = 2026 - 1900;
        t.tm_mon  = 10;
        t.tm_mday = 2;
        t.tm_hour = 9;
        t.tm_isdst = 0;
        struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock not set: default 2026-11-02 09:00 EAT");
    }
}

esp_err_t system_sync_init(void)
{
    if (g_system_events) return ESP_OK;

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

    /* provide access control with the shared SPI mutex and event queue */
    access_control_set_spi_mutex(g_spi_mutex);
    access_control_set_event_queue(g_access_queue);

    /* the evacuation button is active-low and uses the internal pull-up */
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

static void check_intrusion(bool seen[3], TickType_t last[3])
{
    bool intruder = false;

    if (!is_access_permitted()) {                   /* intrusion checks run outside permitted hours */
        for (int room = 1; room <= 2; room++) {
            if (!lighting_get_motion(room)) continue;
            intruder = true;
            TickType_t now = xTaskGetTickCount();
            if (!seen[room] ||
                (now - last[room]) >= pdMS_TO_TICKS(INTRUSION_LOG_INTERVAL_MS)) { //50ms
                seen[room] = true;
                last[room] = now;
                schedule_check_intrusion(true);   /* updtae intursion event bit queues an intrusion event for storage */
            }
        }
    }

    if (intruder) xEventGroupSetBits(g_system_events, SYS_EVT_INTRUSION);
    else          xEventGroupClearBits(g_system_events, SYS_EVT_INTRUSION);
}

/* ================================================================== */
/*  Evacuation tracking: who is inside, who is safe, who is missing    */
/* ================================================================== */
void system_evac_get_report(evac_report_t *r)
{
    memset(r, 0, sizeof(*r));
    access_control_get_evac_summary(&r->people);
    for (int i = 0; i < 2; i++) {
        r->motion[i]       = lighting_get_motion(i + 1);
        r->unregistered[i] = r->motion[i] && (r->people.in_room[i] == 0);
    }
    r->all_clear = (r->people.inside == 0) && !r->motion[0] && !r->motion[1];
}

typedef struct {
    bool       clear_posted;      /* "building empty" already announced   */
    bool       unreg_posted[2];   /* "motion but nobody registered" posted */
    TickType_t next_status;       /* when to post the next progress line  */
} evac_state_t;

static void evac_since_str(time_t t, char *out, size_t n)
{
    if (t == 0) { snprintf(out, n, "--:--"); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
}

static void evac_post_status(const char *tag, const evac_report_t *r)
{
    system_log_post(SYSLOG_EVAC_ROSTER,
                    "%s: safe %d/%d | inside %d (lobby %d R1 %d R2 %d) | missing %d",
                    tag, r->people.safe, r->people.expected, r->people.inside,
                    r->people.in_lobby, r->people.in_room[0], r->people.in_room[1],
                    r->people.missing);
}

/* One log line per person: "<tag> Student A: ROOM1 since 09:12" */
static void evac_post_people(system_log_type_t type, const char *tag, bool only_unaccounted)
{
    person_status_t p[EVAC_MAX_PEOPLE];
    int n = access_control_get_roster(p, EVAC_MAX_PEOPLE);
    for (int i = 0; i < n; i++) {
        if (!p[i].expected) continue;
        if (only_unaccounted && p[i].safe) continue;
        char hm[16];
        evac_since_str(p[i].since, hm, sizeof(hm));
        system_log_post(type, "%s %s: %s since %s", tag, p[i].name,
                        access_control_loc_str(p[i].location), hm);
    }
}

static void evac_post_unregistered(const evac_report_t *r, const char *tag)
{
    for (int i = 0; i < 2; i++) {
        if (r->unregistered[i])
            system_log_post(SYSLOG_EVAC_MISSING,
                            "%s: motion in ROOM%d but nobody registered there - possible untagged person",
                            tag, i + 1);
    }
}

/* Called every monitor cycle while an evacuation is running. */
static void evac_track(evac_state_t *st)
{
    evac_report_t rep;
    system_evac_get_report(&rep);

    if (rep.all_clear && !st->clear_posted) {
        st->clear_posted = true;
        xEventGroupSetBits(g_system_events, SYS_EVT_EVAC_ALL_CLEAR);
        system_log_post(SYSLOG_EVAC_ALL_CLEAR, "BUILDING EMPTY: all %d people accounted for",
                        rep.people.expected);
    } else if (!rep.all_clear && st->clear_posted) {
        st->clear_posted = false;
        xEventGroupClearBits(g_system_events, SYS_EVT_EVAC_ALL_CLEAR);
        system_log_post(SYSLOG_EVAC_MISSING, "Building no longer empty: someone detected inside");
    }

    for (int i = 0; i < 2; i++) {       /* post each untagged-motion alarm once per episode */
        if (rep.unregistered[i] && !st->unreg_posted[i]) {
            st->unreg_posted[i] = true;
            evac_report_t only = rep;
            only.unregistered[1 - i] = false;
            evac_post_unregistered(&only, "EVAC ALERT");
        } else if (!rep.unregistered[i]) {
            st->unreg_posted[i] = false;
        }
    }

    TickType_t now = xTaskGetTickCount();
    if (!rep.all_clear && (int32_t)(now - st->next_status) >= 0) {
        evac_post_status("EVAC STATUS", &rep);
        st->next_status = now + pdMS_TO_TICKS(EVAC_STATUS_PERIOD_MS);
    }
}

/* Evacuation just ended: log the final result BEFORE the building is reset to empty. */
static void evac_finish(void)
{
    evac_report_t rep;
    system_evac_get_report(&rep);

    if (rep.people.missing == 0 && rep.people.inside == 0) {
        system_log_post(SYSLOG_EVAC_ALL_CLEAR, "EVAC END: all %d people accounted for",
                        rep.people.expected);
    } else {
        system_log_post(SYSLOG_EVAC_MISSING, "EVAC END: %d of %d NOT accounted for (still inside %d)",
                        rep.people.missing, rep.people.expected, rep.people.inside);
        evac_post_people(SYSLOG_EVAC_MISSING, "MISSING", true);
    }
    evac_post_unregistered(&rep, "EVAC END");
}

static void vTaskSystemMonitor(void *pvParameters)
{
    (void)pvParameters;
    bool       evac_applied = false;
    evac_state_t evac_st    = { 0 };
    int        btn_prev     = 1;
    bool       seen[3]      = { false, false, false };
    TickType_t last[3]      = { 0, 0, 0 };

    ESP_LOGI(TAG, "system monitor running");

    for (;;) {
        /* toggle evacuation on the button's falling edge */
        int btn = gpio_get_level(EVAC_BUTTON_PIN);
        if (btn == 0 && btn_prev == 1) {
            system_evacuation_set(!system_evacuation_active(), "button");
        }
        btn_prev = btn;

        /* propagate evacuation state changes */
        bool evac = system_evacuation_active();
        if (evac != evac_applied) {
            if (evac) {
                access_control_set_evacuation(true);      /* doors open + snapshot of who is inside */
                lighting_set_evacuation(true);
                evac_report_t rep;
                system_evac_get_report(&rep);
                evac_post_status("EVAC START", &rep);
                evac_post_people(SYSLOG_EVAC_ROSTER, "EVAC ROSTER", false);
                memset(&evac_st, 0, sizeof(evac_st));
                evac_st.next_status = xTaskGetTickCount() + pdMS_TO_TICKS(EVAC_STATUS_PERIOD_MS);
            } else {
                evac_finish();                            /* report BEFORE the reset below */
                access_control_set_evacuation(false);     /* doors close, building -> EMPTY */
                lighting_set_evacuation(false);
                xEventGroupClearBits(g_system_events, SYS_EVT_EVAC_ALL_CLEAR);
                system_log_post(SYSLOG_INFO, "Building reset: all occupants cleared, headcount 0");
            }
            evac_applied = evac;
        }
        if (evac_applied) evac_track(&evac_st);

        /* check for intrusion */
        check_intrusion(seen, last);

        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

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
    ok &= spawn(vTaskSerialCLI,       "cli",      STACK_CLI,      PRIO_CLI);

    if (!ok) return ESP_FAIL;
    ESP_LOGI(TAG, "all tasks started");
    return ESP_OK;
}