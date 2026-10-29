/*
 * access_control.h  --  MEMBER 1: Main Entrance & RFID Access Control
 *
 * Public interface of the access-control module. Other members only ever
 * need THIS header; they never touch the RC522 / servo details.
 */
#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define AC_UID_LEN   4      /* we support 4-byte MIFARE Classic UIDs   */
#define AC_NAME_LEN  16

/* Which door / reader a scan happened at */
typedef enum {
    GATE_ENTRY = 0,
    GATE_EXIT  = 1,
    GATE_COUNT
} gate_t;

/* Outcome of a scan */
typedef enum {
    ACCESS_GRANTED = 0,
    ACCESS_DENIED_UNKNOWN_TAG,     /* UID not in whitelist              */
    ACCESS_DENIED_SCHEDULE,        /* outside allowed building hours    */
    ACCESS_DENIED_ALREADY_INSIDE,  /* anti-passback: entered twice      */
    ACCESS_DENIED_NOT_INSIDE       /* anti-passback: exit without entry */
} access_result_t;

/*
 * One scan event. THIS STRUCT IS THE CONTRACT with Member 3 (queue) and
 * Member 4 (SD logging): Member 3 creates a queue of access_event_t and
 * Member 4 reads from it and writes each event to access_log.txt.
 */
typedef struct {
    uint8_t         uid[AC_UID_LEN];
    char            name[AC_NAME_LEN];   /* "" if unknown tag */
    gate_t          gate;
    access_result_t result;
    time_t          timestamp;
    int             headcount;           /* people inside AFTER this event */
} access_event_t;

/* ---- Lifecycle ---------------------------------------------------- */
/* Init SPI bus (VSPI), both RC522 readers, both door servos. */
esp_err_t access_control_init(void);

/* FreeRTOS task body. Member 3 spawns it:
 *   xTaskCreate(vTaskAccessControl, "access", 4096, NULL, 5, NULL);  */
void vTaskAccessControl(void *pvParameters);

/* ---- Hooks wired up by Member 3 (optional: NULL = feature disabled) */
void access_control_set_event_queue(QueueHandle_t q);    /* xAccessEventQueue */
void access_control_set_spi_mutex(SemaphoreHandle_t m);  /* xSPIBusMutex      */

/* ---- Read-only state for Member 5's CLI (thread-safe) -------------- */
int  access_control_get_headcount(void);
/* Copies the names of people currently inside; returns how many. */
int  access_control_list_inside(char names[][AC_NAME_LEN], int max);

/* ---- Admin override (Member 5 CLI: e.g. "open-door entry") --------- */
void access_control_open_door(gate_t gate, uint32_t hold_ms);

/* ---- Provided by Member 4 (schedule.c) ----------------------------- */
/* Returns true if the building is open right now. A weak default that
 * always returns true lives in access_control.c so you can test alone.  */
bool is_access_permitted(void);

#endif /* ACCESS_CONTROL_H */