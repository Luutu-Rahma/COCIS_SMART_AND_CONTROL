/*
 * lock order: g_config_mutex -> g_sd_mutex -> g_spi_mutex
 * never block or delay while holding a mutex
 * vTaskStorage is the sole consumer of both queues and the only SD writer
 * the system clock uses EAT (UTC+3)
 */
#ifndef SYSTEM_SYNC_H
#define SYSTEM_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "access_control.h"
#include "lighting_control.h"

/* ---------------- Event bits ---------------- */
#define SYS_EVT_EVACUATION   (1u << 0)  /* global emergency override active          */
#define SYS_EVT_INTRUSION    (1u << 1)  /* motion detected while building CLOSED     */
#define SYS_EVT_SD_READY     (1u << 2)  /* set by storage_init() once the SD is mounted */
#define SYS_EVT_EVAC_ALL_CLEAR (1u << 3) /* evacuation running AND the building is empty */

extern EventGroupHandle_t g_system_events;
extern SemaphoreHandle_t  g_spi_mutex;
extern SemaphoreHandle_t  g_sd_mutex;
extern SemaphoreHandle_t  g_config_mutex;
extern QueueHandle_t      g_access_queue;
extern QueueHandle_t      g_syslog_queue;

typedef enum {
    SYSLOG_INFO = 0,
    SYSLOG_EVACUATION_START,
    SYSLOG_EVACUATION_END,
    SYSLOG_INTRUSION,
    SYSLOG_CONFIG_CHANGE,    /* e.g. schedule changed by admin */
    SYSLOG_AUTH_FAIL,        /* failed admin login on the CLI */
    SYSLOG_ADMIN_ACTION,     /* admin command executed (e.g. open-door) */
    SYSLOG_EVAC_ROSTER,      /* evacuation: who is inside / where / progress */
    SYSLOG_EVAC_MISSING,     /* evacuation: person or room NOT accounted for */
    SYSLOG_EVAC_ALL_CLEAR,   /* evacuation: building is empty, all accounted for */
} system_log_type_t;

typedef struct {
    time_t            timestamp;
    system_log_type_t type;
    char              text[96];
} system_log_event_t;

/* ---------------- Evacuation report (who is where, who is missing) ---------------- */
typedef struct {
    evac_summary_t people;          /* counts from the RFID roster                     */
    bool           motion[2];       /* PIR motion now: [0] = room 1, [1] = room 2       */
    bool           unregistered[2]; /* motion in a room where NO tagged person is       */
    bool           all_clear;       /* nobody tagged inside AND no motion in any room   */
} evac_report_t;

/* Fill `r` from the access-control roster + the room PIR sensors. Thread-safe. */
void system_evac_get_report(evac_report_t *r);

/* ---------------- Tunables ---------------- */
#define TASK_CORE            tskNO_AFFINITY   /* change to 0 or 1 to pin tasks */
#define EVAC_BUTTON_PIN      32               /* pushbutton to GND, internal pull-up */

/* call before initializing modules that use shared resources */
esp_err_t system_sync_init(void);

/* call after all module initialization functions */
esp_err_t system_sync_start_tasks(void);

/* set or clear evacuation state; source is included in the log */
void system_evacuation_set(bool active, const char *source);
bool system_evacuation_active(void);

/* non-blocking; returns false if the queue is full */
bool system_log_post(system_log_type_t type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

const char *system_log_type_str(system_log_type_t t);

#endif /* SYSTEM_SYNC_H */