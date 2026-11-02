#pragma once
#include <stdbool.h>

typedef enum { ROLE_STUDENT = 0, ROLE_LECTURER = 1, ROLE_ADMIN = 2 } user_role_t;

void schedule_set_time(int hour, int minute);        // set the clock (ESP32 has no battery clock)
void schedule_set_hours(int open_hour, int close_hour); // change the opening hours
void schedule_get_hours(int *open_hour, int *close_hour); // Member 3: read the opening hours (for the CLI)
bool is_access_permitted(void);                      // true = building open
bool is_access_permitted_for_role(user_role_t role);
void schedule_check_intrusion(bool motion_detected); // motion while closed = alert
