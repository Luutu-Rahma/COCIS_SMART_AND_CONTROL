/*
 * access_control.h  --  MEMBER 1: Main Entrance, Room Doors & RFID Access Control
 *
 * Public interface of the access-control module. Other members only ever
 * need THIS header; they never touch the RC522 / servo details.
 *
 * NEW (evacuation tracking): every person now has a LOCATION (outside / lobby /
 * room 1 / room 2). Four RFID readers feed it: the two main doors plus one reader
 * on each lecture-room door. During an evacuation the module keeps a roster of
 * who was inside when it started and who has reached the exit ("safe"), so
 * rescuers know how many people are missing and where they were last seen.
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

/* The two DOORS that have a servo (unchanged: servo arrays are sized by GATE_COUNT) */
typedef enum {
    GATE_ENTRY = 0,
    GATE_EXIT  = 1,
    GATE_COUNT
} gate_t;

/* Every RFID reader in the building. The first two are the main doors
 * (READER_ENTRY == GATE_ENTRY, READER_EXIT == GATE_EXIT). */
typedef enum {
    READER_ENTRY = 0,   /* main entrance                    */
    READER_EXIT  = 1,   /* main exit                        */
    READER_ROOM1 = 2,   /* lecture room 1 door              */
    READER_ROOM2 = 3,   /* lecture room 2 door              */
    READER_COUNT
} reader_t;

/* Where a person is right now (last place they tapped a card) */
typedef enum {
    LOC_OUTSIDE = 0,    /* not in the building (or already evacuated) */
    LOC_LOBBY,          /* inside the building, not in a lecture room */
    LOC_ROOM1,
    LOC_ROOM2
} person_loc_t;

/* Outcome of a scan (new values are appended so old numbers do not change) */
typedef enum {
    ACCESS_GRANTED = 0,
    ACCESS_DENIED_UNKNOWN_TAG,     /* UID not in whitelist              */
    ACCESS_DENIED_SCHEDULE,        /* outside allowed building hours    */
    ACCESS_DENIED_ALREADY_INSIDE,  /* anti-passback: entered twice      */
    ACCESS_DENIED_NOT_INSIDE,      /* anti-passback: exit without entry */
    ACCESS_DENIED_EVACUATION,      /* no re-entry while evacuating      */
    ACCESS_EVAC_SAFE               /* evacuation: person reached the exit */
} access_result_t;

/*
 * One scan event. THIS STRUCT IS THE CONTRACT with Member 3 (queue) and
 * Member 4 (SD logging): Member 3 creates a queue of access_event_t and
 * Member 4 reads from it and writes each event to access_log.txt.
 * (`gate` became `reader` and `location` was added for evacuation tracking.)
 */
typedef struct {
    uint8_t         uid[AC_UID_LEN];
    char            name[AC_NAME_LEN];   /* "" if unknown tag */
    reader_t        reader;              /* which reader was tapped */
    access_result_t result;
    time_t          timestamp;
    int             headcount;           /* people inside AFTER this event */
    person_loc_t    location;            /* where the person is AFTER this event */
} access_event_t;

/* ---- Evacuation roster -------------------------------------------- */
typedef struct {
    char         name[AC_NAME_LEN];
    person_loc_t location;
    time_t       since;       /* when `location` last changed (0 = never) */
    bool         expected;    /* was inside when the evacuation started   */
    bool         safe;        /* has tapped the exit since it started     */
} person_status_t;

typedef struct {
    int inside;        /* people inside the building right now             */
    int in_lobby;
    int in_room[2];    /* [0] = room 1, [1] = room 2                       */
    int expected;      /* inside when the evacuation started               */
    int safe;          /* of those, how many have reached the exit         */
    int missing;       /* expected but not safe yet = still to be rescued  */
} evac_summary_t;

/* ---- Lifecycle ---------------------------------------------------- */
/* Init SPI bus (VSPI), the four RC522 readers, both door servos. */
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
/* Copies the status of every known person (name, location, safe...); returns how many. */
int  access_control_get_roster(person_status_t *out, int max);
/* Counts for the evacuation dashboard (all zero except `inside*` when no evacuation). */
void access_control_get_evac_summary(evac_summary_t *s);
/* "OUTSIDE", "LOBBY", "ROOM1", "ROOM2" (no spaces/commas: safe for the CSV log) */
const char *access_control_loc_str(person_loc_t loc);

/* ---- Admin override (Member 5 CLI: e.g. "open-door entry") --------- */
void access_control_open_door(gate_t gate, uint32_t hold_ms);

/*
 * Evacuation switch (called by the system monitor).
 *  true : doors held open, snapshot of who is inside, exit taps mark people SAFE.
 *  false: doors close and the building is RESET TO EMPTY (everybody outside,
 *         headcount 0). The monitor logs the final report BEFORE calling this.
 */
void access_control_set_evacuation(bool active);

/* ---- Provided by Member 4 (schedule.c) ----------------------------- */
/* Returns true if the building is open right now. A weak default that
 * always returns true lives in access_control.c so you can test alone.  */
bool is_access_permitted(void);

#endif /* ACCESS_CONTROL_H */