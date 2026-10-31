/*
 * system_sync.h -- MEMBER 3: shared synchronization primitives + module contract
 *
 * Single ESP32, many FreeRTOS tasks. The ESP32 is dual-core, so tasks can run
 * truly in parallel: every shared resource below MUST be protected.
 *
 * LOCK ORDER (always acquire in this order, never the reverse, never skip back):
 *      g_config_mutex  ->  g_sd_mutex  ->  g_spi_mutex
 *   - g_spi_mutex   : the shared SPI3 bus (RFID readers + SD card)
 *   - g_sd_mutex    : the FAT filesystem / log file
 *   - g_config_mutex: schedule settings + admin credentials
 * Never call a blocking function (queue send with timeout, vTaskDelay) while
 * holding a mutex. Keep critical sections short.
 *
 * QUEUES
 *   g_access_queue : access_event_t        (producer: access_control, consumer: vTaskStorage)
 *   g_syslog_queue : system_log_event_t    (producers: anyone via system_log_post(),
 *                                           consumer: vTaskStorage)
 *   vTaskStorage is the ONLY consumer of both queues (single writer to the SD card).
 *
 * EVENT GROUP g_system_events: SYS_EVT_* bits below.
 *
 * CLOCK: the system clock (time(NULL)) is the single time source. TZ is EAT (UTC+3);
 * use localtime_r() for hours/minutes. It is set to a default at boot
 * (system_sync_init) and can be changed with settimeofday() (CLI `set-time`).
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
#define SYS_EVT_SD_READY     (1u << 2)  /* set by data_storage once the SD is mounted */

/* ---------------- Shared handles (created by system_sync_init) ---------------- */
extern EventGroupHandle_t g_system_events;
extern SemaphoreHandle_t  g_spi_mutex;
extern SemaphoreHandle_t  g_sd_mutex;
extern SemaphoreHandle_t  g_config_mutex;
extern QueueHandle_t      g_access_queue;   /* access_event_t */
extern QueueHandle_t      g_syslog_queue;   /* system_log_event_t */

/* ---------------- System log events ---------------- */
typedef enum {
    SYSLOG_INFO = 0,
    SYSLOG_EVACUATION_START,
    SYSLOG_EVACUATION_END,
    SYSLOG_INTRUSION,
    SYSLOG_CONFIG_CHANGE,    /* e.g. schedule changed by admin */
} system_log_type_t;

typedef struct {
    time_t            timestamp;
    system_log_type_t type;
    char              text[96];
} system_log_event_t;

/* ---------------- Tunables ---------------- */
#define TASK_CORE            tskNO_AFFINITY   /* change to 0 or 1 to pin tasks */
#define EVAC_BUTTON_PIN      32               /* pushbutton to GND, internal pull-up */

/* ---------------- API (Member 3) ---------------- */

/* Create mutexes/queues/event group, inject them into access_control, set default
 * clock, configure the evacuation button. Call FIRST in app_main. */
esp_err_t system_sync_init(void);

/* Create all FreeRTOS tasks. Call LAST in app_main (after every *_init). */
esp_err_t system_sync_start_tasks(void);

/* Trigger / clear the global evacuation override. Safe from any task (CLI, etc).
 * `source` is free text for the log ("cli", "button"). */
void system_evacuation_set(bool active, const char *source);
bool system_evacuation_active(void);

/* Post a message to the log queue (non-blocking, printf-style). Returns false if full. */
bool system_log_post(system_log_type_t type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

const char *system_log_type_str(system_log_type_t t);

#endif /* SYSTEM_SYNC_H */