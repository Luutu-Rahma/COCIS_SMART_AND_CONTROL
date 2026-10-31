#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "schedule.h"
#include "data_storage.h"

static int open_hour  = 7;    // building opens at 07:00
static int close_hour = 22;   // building closes at 22:00

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
    open_hour  = open_h;
    close_hour = close_h;
}

// Returns true if the current hour is inside the opening hours
bool is_access_permitted(void)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_hour >= open_hour && t.tm_hour < close_hour);
}

// Called when a PIR sensor reports motion
void schedule_check_intrusion(bool motion_detected)
{
    if (motion_detected && !is_access_permitted()) {
        printf("INTRUSION ALERT!\n");
        log_event_to_sd("INTRUSION_ALERT", "PIR");
        // later: turn on the red LED / buzzer here
    }
}