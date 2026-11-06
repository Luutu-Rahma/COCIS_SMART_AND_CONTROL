#pragma once
#include <stdbool.h>

typedef enum { ROLE_STUDENT = 0, ROLE_LECTURER = 1, ROLE_ADMIN = 2 } user_role_t;

void schedule_set_time(int hour, int minute);        // ESP32 has no battery-backed clock
void schedule_set_hours(int open_hour, int close_hour);
void schedule_get_hours(int *open_hour, int *close_hour);
bool is_access_permitted(void);                      // true when the building is open
bool is_access_permitted_for_role(user_role_t role);
void schedule_check_intrusion(bool motion_detected); // alert on motion outside permitted hours
