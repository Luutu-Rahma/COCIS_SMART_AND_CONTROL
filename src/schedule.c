#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "schedule.h"
#include "data_storage.h"
#include "system_sync.h"   // Member 3: g_config_mutex, system_log_post

#define PIN_ALERT_LED 16
#define PIN_BUZZER    17

static int open_hour  = 7;    // building opens at 07:00
static int close_hour = 22;   // building closes at 22:00

// Intrusion alert outputs
static void alert_set(bool on)   
{
    static bool ready = false;
    if (!ready) {
        gpio_set_direction(PIN_ALERT_LED, GPIO_MODE_OUTPUT);
        gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
        ready = true;
    }
    gpio_set_level(PIN_ALERT_LED, on);
    gpio_set_level(PIN_BUZZER, on);
}


// Member 3: shared configuration is protected by g_config_mutex.
static void cfg_lock(void)   { if (g_config_mutex) xSemaphoreTake(g_config_mutex, portMAX_DELAY); }
static void cfg_unlock(void) { if (g_config_mutex) xSemaphoreGive(g_config_mutex); }

// Set today's clock time.
void schedule_set_time(int hour, int minute)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_year = 126;
    t.tm_mon  = 10;
    t.tm_mday = 1;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = 0;

    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

// Update opening hours from the admin CLI.
void schedule_set_hours(int open_h, int close_h)
{
    cfg_lock();
    open_hour  = open_h;
    close_hour = close_h;
    cfg_unlock();
    // Member 3: configuration audit log.
    system_log_post(SYSLOG_CONFIG_CHANGE, "Opening hours set to %02d:00-%02d:00", open_h, close_h);
}

// Read opening hours for the CLI.
void schedule_get_hours(int *open_h, int *close_h)
{
    cfg_lock();
    if (open_h)  *open_h  = open_hour;
    if (close_h) *close_h = close_hour;
    cfg_unlock();
}

// Check whether the building is open.
bool is_access_permitted(void)
{
    int o, c;
    cfg_lock();
    o = open_hour;
    c = close_hour;
    cfg_unlock();

    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_hour >= o && t.tm_hour < c);
}

bool is_access_permitted_for_role(user_role_t role)
{
    if (role == ROLE_LECTURER || role == ROLE_ADMIN) return true;
    return is_access_permitted();
}


// Handle PIR motion reports.
void schedule_check_intrusion(bool motion_detected)
{
    if (motion_detected && !is_access_permitted()) {
        printf("INTRUSION ALERT!\n");
        system_log_post(SYSLOG_INTRUSION, "Motion detected while building CLOSED"); 
        log_event_to_sd("INTRUSION_ALERT", "PIR");
        alert_set(true);     // LED and buzzer on
    } else {
        alert_set(false);    // LED and buzzer off
    }
}
