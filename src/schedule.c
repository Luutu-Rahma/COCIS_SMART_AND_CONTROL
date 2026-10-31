#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "schedule.h"
#include "data_storage.h"
#include "system_sync.h"   // Member 3: g_config_mutex, system_log_post

static int open_hour  = 7;    // building opens at 07:00
static int close_hour = 22;   // building closes at 22:00

// Member 3: the hours are read by two tasks and written by the CLI, so every access
// is guarded by g_config_mutex (it is NULL until system_sync_init() has run).
static void cfg_lock(void)   { if (g_config_mutex) xSemaphoreTake(g_config_mutex, portMAX_DELAY); }
static void cfg_unlock(void) { if (g_config_mutex) xSemaphoreGive(g_config_mutex); }

// Set the clock to a chosen time today (used for demo/testing)
void schedule_set_time(int hour, int minute)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_year = 126;          // years since 1900, so 2026
    t.tm_mon  = 10;           // months start at 0, so 10 = November
    t.tm_mday = 1;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = 0;

    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

// The admin CLI (Member 5) can call this to change the hours
void schedule_set_hours(int open_h, int close_h)
{
    cfg_lock();
    open_hour  = open_h;
    close_hour = close_h;
    cfg_unlock();
    // Member 3: audit trail of the configuration change
    system_log_post(SYSLOG_CONFIG_CHANGE, "Opening hours set to %02d:00-%02d:00", open_h, close_h);
}

// Member 3: read the current hours (for the CLI status/menu)
void schedule_get_hours(int *open_h, int *close_h)
{
    cfg_lock();
    if (open_h)  *open_h  = open_hour;
    if (close_h) *close_h = close_hour;
    cfg_unlock();
}

// Returns true if the current hour is inside the opening hours
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

// Called when a PIR sensor reports motion
void schedule_check_intrusion(bool motion_detected)
{
    if (motion_detected && !is_access_permitted()) {
        printf("INTRUSION ALERT!\n");
        // Member 3: post to the log queue; vTaskStorage is the single writer to the SD card
        system_log_post(SYSLOG_INTRUSION, "Motion detected while building CLOSED");
        // later: turn on the red LED / buzzer here
    }
}
