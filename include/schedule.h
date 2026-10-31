#pragma once
#include <stdbool.h>

void schedule_set_time(int hour, int minute);        // set the clock (ESP32 has no battery clock)
void schedule_set_hours(int open_hour, int close_hour); // change the opening hours
bool is_access_permitted(void);                      // true = building open
void schedule_check_intrusion(bool motion_detected); // motion while closed = alert