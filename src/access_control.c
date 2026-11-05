/*
 * access_control.c  --  MEMBER 1: Main Entrance & RFID Access Control
 *
 * What this file does:
 *   1. Talks to TWO MFRC522 (RC522) RFID readers over ONE shared SPI bus.
 *   2. Reads the 4-byte UID of a tag, checks it against a whitelist.
 *   3. Opens the correct door servo using PWM (LEDC) for a few seconds.
 *   4. Keeps the live headcount and WHERE each person is (outside / lobby /
 *      room 1 / room 2), using 4 readers: entry, exit, room 1 door, room 2 door.
 *   5. Emits an access_event_t to a queue so Member 4 can log it to the SD card.
 *   6. EVACUATION: remembers who was inside when it started, marks people SAFE
 *      when they tap the exit reader, and EMPTIES the building when it ends.
 *
 * Pin map (matches the Wokwi diagram):
 *   SPI bus (VSPI):  SCK=18  MISO=19  MOSI=23     Shared RST: 15
 *   Chip selects:    ENTRY=5  EXIT=4  ROOM1=GPIO0  ROOM2=GPIO12
 *   Entry servo PWM: 33              Exit servo PWM: 27
 */
#include "access_control.h"

#include <string.h>
#include <stdio.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "schedule.h"
#include "freertos/task.h"

static const char *TAG = "ACCESS";

/* ------------------------------------------------------------------ */
/*  Pins & constants                                                   */
/* ------------------------------------------------------------------ */
#define RC522_SPI_HOST   SPI3_HOST      /* VSPI on the ESP32 */
#define PIN_SPI_SCK      18
#define PIN_SPI_MISO     19
#define PIN_SPI_MOSI     23
#define PIN_CS_ENTRY     5
#define PIN_CS_EXIT      4
#define PIN_CS_ROOM1     0      /* lecture room 1 door reader (GPIO 0 = boot pin, keep idle HIGH) */
#define PIN_CS_ROOM2     12     /* lecture room 2 door reader (GPIO 12 = strapping pin)          */
#define PIN_RC522_RST    15
#define PIN_SERVO_ENTRY  33
#define PIN_SERVO_EXIT   27

#define DOOR_OPEN_MS     3000           /* how long a door stays open */
#define SAME_TAG_LOCKOUT_US (2LL * 1000000) /* ignore same tag for 2 s */
#define POLL_PERIOD_MS   100

#define SERVO_ANGLE_CLOSED  0
#define SERVO_ANGLE_OPEN    90

/* ------------------------------------------------------------------ */
/*  MFRC522 register map (only what we use)                            */
/* ------------------------------------------------------------------ */
#define REG_COMMAND       0x01
#define REG_COM_IRQ       0x04
#define REG_DIV_IRQ       0x05
#define REG_ERROR         0x06
#define REG_FIFO_DATA     0x09
#define REG_FIFO_LEVEL    0x0A
#define REG_CONTROL       0x0C
#define REG_BIT_FRAMING   0x0D
#define REG_COLL          0x0E
#define REG_MODE          0x11
#define REG_TX_CONTROL    0x14
#define REG_TX_ASK        0x15
#define REG_CRC_RESULT_H  0x21
#define REG_CRC_RESULT_L  0x22
#define REG_T_MODE        0x2A
#define REG_T_PRESCALER   0x2B
#define REG_T_RELOAD_H    0x2C
#define REG_T_RELOAD_L    0x2D
#define REG_VERSION       0x37

/* RC522 commands (written to REG_COMMAND) */
#define CMD_IDLE          0x00
#define CMD_CALC_CRC      0x03
#define CMD_TRANSCEIVE    0x0C
#define CMD_SOFT_RESET    0x0F

/* ISO14443A card commands */
#define PICC_REQA         0x26
#define PICC_SEL_CL1      0x93
#define PICC_HLTA         0x50

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */
static spi_device_handle_t s_rc522[READER_COUNT];
static bool s_reader_ok[READER_COUNT];   /* false = reader did not answer at boot (not wired) */

static SemaphoreHandle_t s_spi_mutex   = NULL;  /* from Member 3 (optional) */
static QueueHandle_t     s_event_queue = NULL;  /* from Member 3 (optional) */
static SemaphoreHandle_t s_state_mutex = NULL;  /* OURS: guards state below */

/* --- Whitelist ---------------------------------------------------- */
typedef struct {
    uint8_t      uid[AC_UID_LEN];
    const char  *name;
    person_loc_t loc;             /* LOC_OUTSIDE = not inside (also the anti-passback flag) */
    user_role_t  role;            /* role from schedule.h */
    time_t       since;           /* when `loc` last changed */
    bool         expected;        /* evacuation: was inside when it started */
    bool         safe;            /* evacuation: has reached the exit       */
} user_t;

typedef struct {
    uint8_t uid[4];
    const char *name;
    user_role_t role;
} whitelist_entry_t;       //the strust name may differ

/*
 * Wokwi's built-in cards (click the RFID reader -> pick a card):
 *   Blue 01:02:03:04   Green 11:22:33:44   Yellow 55:66:77:88
 *   Red  AA:BB:CC:DD   Key fob C0:FF:EE:99
 *   (the grey "NFC Tag" has a 7-byte UID and is NOT supported by this driver)
 * Red is deliberately left OUT so you can test "unknown tag".
 */
static user_t s_users[] = {
    { .uid = {0x01, 0x02, 0x03, 0x04}, .name = "Student A",  .loc = LOC_OUTSIDE, .role = ROLE_STUDENT  },   /* Blue   */
    { .uid = {0x11, 0x22, 0x33, 0x44}, .name = "Student B",  .loc = LOC_OUTSIDE, .role = ROLE_STUDENT  },   /* Green  */
    { .uid = {0x55, 0x66, 0x77, 0x88}, .name = "Lecturer",   .loc = LOC_OUTSIDE, .role = ROLE_LECTURER },   /* Yellow */
    { .uid = {0xC0, 0xFF, 0xEE, 0x99}, .name = "Security",   .loc = LOC_OUTSIDE, .role = ROLE_ADMIN    },   /* Key fob */
};
#define NUM_USERS (sizeof(s_users) / sizeof(s_users[0]))

static int s_headcount = 0;
static bool s_evacuation = false;

/* --- Doors -------------------------------------------------------- */
typedef struct {
    bool    open;
    int64_t close_at_us;
} door_t;
static door_t s_doors[GATE_COUNT];

static const int           s_servo_pin[GATE_COUNT] = { PIN_SERVO_ENTRY, PIN_SERVO_EXIT };
static const ledc_channel_t s_servo_ch[GATE_COUNT] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };
static const int           s_cs_pin[READER_COUNT]  = { PIN_CS_ENTRY, PIN_CS_EXIT, PIN_CS_ROOM1, PIN_CS_ROOM2 };

static const char *reader_name(reader_t r)
{
    switch (r) {
    case READER_ENTRY: return "ENTRY";
    case READER_EXIT:  return "EXIT";
    case READER_ROOM1: return "ROOM1";
    case READER_ROOM2: return "ROOM2";
    default:           return "?";
    }
}

/*
 * IMPORTANT (ESP32 limit): one SPI host has only 3 hardware chip-select slots, and any
 * SPI device created with spics_io_num >= 0 takes one. ENTRY + EXIT + the SD card already
 * use all 3, so asking for a 4th makes spi_bus_add_device() fail with ESP_ERR_NO_MEM
 * (boot loop). The two ROOM readers therefore use a SOFTWARE chip-select: ONE shared SPI
 * device with spics_io_num = -1, and the firmware pulls the reader's CS GPIO low around
 * every transaction itself (see rc522_xfer). -1 = "this reader uses the hardware CS".
 */
static const int s_soft_cs[READER_COUNT] = { -1, -1, PIN_CS_ROOM1, PIN_CS_ROOM2 };

/* ------------------------------------------------------------------ */
/*  Weak default for Member 4's function                               */
/* ------------------------------------------------------------------ */
__attribute__((weak)) bool is_access_permitted(void)
{
    return true;  /* replaced automatically when schedule.c defines it */
}

/* ================================================================== */
/*  LAYER 1: raw SPI register access to the RC522                      */
/* ================================================================== */
/*
 * RC522 SPI frame: first byte = address byte, second byte = data.
 *   address byte = (reg << 1) & 0x7E, and bit7 = 1 for a READ.
 * For a read we clock out a dummy 0x00 and the chip answers in byte 2.
 */
/* One SPI frame to reader g. For the room readers the CS GPIO is driven by hand. */
static void rc522_xfer(reader_t g, spi_transaction_t *t)
{
    if (s_soft_cs[g] >= 0) gpio_set_level(s_soft_cs[g], 0);
    esp_err_t e = spi_device_transmit(s_rc522[g], t);
    if (s_soft_cs[g] >= 0) gpio_set_level(s_soft_cs[g], 1);
    (void)e;
}

static void rc522_write(reader_t g, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    rc522_xfer(g, &t);
}

static uint8_t rc522_read(reader_t g, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    rc522_xfer(g, &t);
    return rx[1];
}

static void rc522_set_bits(reader_t g, uint8_t reg, uint8_t mask)
{
    rc522_write(g, reg, rc522_read(g, reg) | mask);
}

static void rc522_clear_bits(reader_t g, uint8_t reg, uint8_t mask)
{
    rc522_write(g, reg, rc522_read(g, reg) & (uint8_t)~mask);
}

/*
 * FIFO access MUST be done as ONE burst transaction (this is how the standard
 * MFRC522 library does it). Reading the FIFO one byte per transaction makes the
 * reader pop TWO bytes each time, which corrupted the UID.
 *   burst read : tx = [addr|0x80] x n, then 0x00 ; data = rx[1..n]
 *   burst write: tx = [addr, byte0, byte1, ...]
 */
#define FIFO_MAX 16

static void rc522_write_fifo(reader_t g, const uint8_t *data, uint8_t len)
{
    uint8_t tx[FIFO_MAX + 1];
    if (len > FIFO_MAX) len = FIFO_MAX;
    tx[0] = (uint8_t)((REG_FIFO_DATA << 1) & 0x7E);
    memcpy(&tx[1], data, len);
    spi_transaction_t t = { .length = (size_t)(len + 1) * 8, .tx_buffer = tx };
    rc522_xfer(g, &t);
}

static void rc522_read_fifo(reader_t g, uint8_t *buf, uint8_t n)
{
    uint8_t tx[FIFO_MAX + 1], rx[FIFO_MAX + 1];
    if (n == 0) return;
    if (n > FIFO_MAX) n = FIFO_MAX;
    uint8_t addr = (uint8_t)(((REG_FIFO_DATA << 1) & 0x7E) | 0x80);
    for (uint8_t i = 0; i < n; i++) tx[i] = addr;
    tx[n] = 0x00;
    memset(rx, 0, sizeof(rx));
    spi_transaction_t t = { .length = (size_t)(n + 1) * 8, .tx_buffer = tx, .rx_buffer = rx };
    rc522_xfer(g, &t);
    memcpy(buf, &rx[1], n);
}

/* ================================================================== */
/*  LAYER 2: talking to a card through the RC522                       */
/* ================================================================== */

/* Ask the RC522's hardware to compute the ISO14443 CRC_A of a buffer. */
static bool rc522_calc_crc(reader_t g, const uint8_t *data, uint8_t len, uint8_t out[2])
{
    rc522_write(g, REG_COMMAND, CMD_IDLE);
    rc522_write(g, REG_DIV_IRQ, 0x04);           /* clear CRCIRq flag  */
    rc522_write(g, REG_FIFO_LEVEL, 0x80);        /* flush FIFO         */
    rc522_write_fifo(g, data, len);
    rc522_write(g, REG_COMMAND, CMD_CALC_CRC);

    int64_t deadline = esp_timer_get_time() + 89000;
    bool done = false;
    while (esp_timer_get_time() < deadline) {
        if (rc522_read(g, REG_DIV_IRQ) & 0x04) { done = true; break; }
    }
    rc522_write(g, REG_COMMAND, CMD_IDLE);
    if (!done) return false;
    out[0] = rc522_read(g, REG_CRC_RESULT_L);
    out[1] = rc522_read(g, REG_CRC_RESULT_H);
    return true;
}

/*
 * Send bytes to the card and collect its reply.
 *   tx_last_bits: number of valid bits in the last tx byte (0 = all 8,
 *                 7 is used for the short REQA frame).
 */
static bool rc522_transceive(reader_t g, const uint8_t *tx, uint8_t tx_len,
                             uint8_t *rx, uint8_t *rx_len, uint8_t tx_last_bits)
{
    rc522_write(g, REG_COMMAND, CMD_IDLE);       /* stop any command     */
    rc522_write(g, REG_COM_IRQ, 0x7F);           /* clear all IRQ flags  */
    rc522_write(g, REG_FIFO_LEVEL, 0x80);        /* flush FIFO           */
    rc522_write_fifo(g, tx, tx_len);
    rc522_write(g, REG_BIT_FRAMING, tx_last_bits);
    rc522_write(g, REG_COMMAND, CMD_TRANSCEIVE);
    rc522_set_bits(g, REG_BIT_FRAMING, 0x80);    /* StartSend            */

    /* Wait for: RxIRq|IdleIRq (0x30) = got reply, TimerIRq (0x01) = none */
    int64_t deadline = esp_timer_get_time() + 36000;
    bool got_reply = false;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq = rc522_read(g, REG_COM_IRQ);
        if (irq & 0x30) { got_reply = true; break; }
        if (irq & 0x01) break;                   /* timeout: no card     */
    }
    rc522_clear_bits(g, REG_BIT_FRAMING, 0x80);  /* StopSend             */
    if (!got_reply) return false;
    if (rc522_read(g, REG_ERROR) & 0x13) return false; /* buf ovf/parity/protocol */

    uint8_t n = rc522_read(g, REG_FIFO_LEVEL);
    if (rx && rx_len) {
        if (n > *rx_len) return false;
        rc522_read_fifo(g, rx, n);
        *rx_len = n;
    }
    return true;
}

/* Step 1: "Is any card in the field?"  (REQA -> card answers with ATQA) */
static bool picc_request(reader_t g)
{
    rc522_clear_bits(g, REG_COLL, 0x80);
    uint8_t cmd = PICC_REQA, atqa[2], len = sizeof(atqa);
    return rc522_transceive(g, &cmd, 1, atqa, &len, 7) && len == 2;
}

/* Step 2+3: anti-collision (card sends its UID) then SELECT it. */
static bool picc_read_uid(reader_t g, uint8_t uid[AC_UID_LEN])
{
    /* Anti-collision: NVB=0x20 means "I know 0 bits, send me everything" */
    uint8_t ac_cmd[2] = { PICC_SEL_CL1, 0x20 };
    uint8_t resp[16], len = sizeof(resp);
    bool ok = rc522_transceive(g, ac_cmd, 2, resp, &len, 0);
    if (!ok) {
        ESP_LOGW(TAG, "DBG anticoll: transceive failed (ComIrq=0x%02X Error=0x%02X FIFO=%u)",
                 rc522_read(g, REG_COM_IRQ), rc522_read(g, REG_ERROR), rc522_read(g, REG_FIFO_LEVEL));
        return false;
    }
    ESP_LOGI(TAG, "DBG anticoll: got %u bytes: %02X %02X %02X %02X %02X", len,
             resp[0], resp[1], resp[2], resp[3], resp[4]);
    if (len != 5) return false;

    uint8_t bcc = resp[0] ^ resp[1] ^ resp[2] ^ resp[3];   /* checksum */
    if (bcc != resp[4]) {
        ESP_LOGW(TAG, "DBG anticoll: BCC mismatch (calc 0x%02X, card 0x%02X)", bcc, resp[4]);
        return false;
    }
    if (resp[0] == 0x88) {
        ESP_LOGW(TAG, "DBG anticoll: 7-byte UID card (NFC Tag) - pick another card");
        return false;   /* 7-byte UID (cascade) unsupported */
    }

    /* SELECT: 0x93 0x70 + UID(4) + BCC + CRC_A(2) */
    uint8_t sel[9] = { PICC_SEL_CL1, 0x70, resp[0], resp[1], resp[2], resp[3], resp[4] };
    if (!rc522_calc_crc(g, sel, 7, &sel[7])) {
        ESP_LOGW(TAG, "CRC calc failed, using UID from anti-collision");
        memcpy(uid, resp, AC_UID_LEN);
        return true;
    }
    uint8_t sak[3], sak_len = sizeof(sak);
    if (!rc522_transceive(g, sel, 9, sak, &sak_len, 0) || sak_len < 1) {
        /* UID already passed the BCC check, so accept it anyway */
        ESP_LOGW(TAG, "SELECT step failed, using UID from anti-collision");
    }

    memcpy(uid, resp, AC_UID_LEN);
    return true;
}

/* Put the card to sleep so it is not re-read while it stays in the field */
static void picc_halt(reader_t g)
{
    uint8_t buf[4] = { PICC_HLTA, 0x00 };
    if (!rc522_calc_crc(g, buf, 2, &buf[2])) return;
    rc522_transceive(g, buf, 4, NULL, NULL, 0);   /* card never answers (timeout is normal) */
}

static void rc522_chip_init(reader_t g)
{
    rc522_write(g, REG_COMMAND, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Timer: auto-start, ~25 ms timeout so "no card" returns quickly */
    rc522_write(g, REG_T_MODE,      0x80);
    rc522_write(g, REG_T_PRESCALER, 0xA9);
    rc522_write(g, REG_T_RELOAD_H,  0x03);
    rc522_write(g, REG_T_RELOAD_L,  0xE8);
    rc522_write(g, REG_TX_ASK,      0x40);   /* 100% ASK modulation     */
    rc522_write(g, REG_MODE,        0x3D);   /* CRC preset 0x6363       */
    rc522_set_bits(g, REG_TX_CONTROL, 0x03); /* antenna ON              */

    uint8_t ver = rc522_read(g, REG_VERSION);
    s_reader_ok[g] = !(ver == 0x00 || ver == 0xFF);
    ESP_LOGI(TAG, "%s reader VersionReg = 0x%02X %s",
             reader_name((reader_t)g), ver,
             (ver == 0x00 || ver == 0xFF) ? "<-- NO RESPONSE, CHECK WIRING!" : "(OK)");
}

/* ================================================================== */
/*  LAYER 3: door servos (PWM via LEDC)                                */
/* ================================================================== */
/*
 * A hobby servo wants a 50 Hz signal (20 ms period). The pulse WIDTH sets
 * the angle: ~0.5 ms = 0 deg ... ~2.5 ms = 180 deg.
 * With 14-bit resolution one period = 16384 counts, so
 *      duty = pulse_us * 16384 / 20000
 */
static void servo_write_angle(gate_t g, int angle)
{
    uint32_t pulse_us = 500 + (uint32_t)(angle * 2000) / 180;
    uint32_t duty     = (pulse_us * (1U << 14)) / 20000U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, s_servo_ch[g], duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, s_servo_ch[g]);
}

static void servos_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int g = 0; g < GATE_COUNT; g++) {
        ledc_channel_config_t ch = {
            .gpio_num   = s_servo_pin[g],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = s_servo_ch[g],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
        servo_write_angle((gate_t)g, SERVO_ANGLE_CLOSED);
    }
}

/* Non-blocking door logic: open now, remember WHEN to close. */
void access_control_open_door(gate_t gate, uint32_t hold_ms)
{
    if (gate >= GATE_COUNT) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_evacuation) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "door open request ignored during evacuation");
        return;
    }
    s_doors[gate].open        = true;
    s_doors[gate].close_at_us = esp_timer_get_time() + (int64_t)hold_ms * 1000;
    xSemaphoreGive(s_state_mutex);
    servo_write_angle(gate, SERVO_ANGLE_OPEN);
    ESP_LOGI(TAG, "%s DOOR OPEN for %u ms", gate == GATE_ENTRY ? "ENTRY" : "EXIT", (unsigned)hold_ms);
}

static void doors_update(void)
{
    int64_t now = esp_timer_get_time();
    for (int g = 0; g < GATE_COUNT; g++) {
        bool close_now = false;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!s_evacuation && s_doors[g].open && now >= s_doors[g].close_at_us) {
            s_doors[g].open = false;
            close_now = true;
        }
        xSemaphoreGive(s_state_mutex);
        if (close_now) {
            servo_write_angle((gate_t)g, SERVO_ANGLE_CLOSED);
            ESP_LOGI(TAG, "%s DOOR CLOSED", reader_name((reader_t)g));
        }
    }
}

/* ================================================================== */
/*  LAYER 4: access decision + headcount                               */
/* ================================================================== */
static int find_user(const uint8_t uid[AC_UID_LEN])
{
    for (size_t i = 0; i < NUM_USERS; i++)
        if (memcmp(s_users[i].uid, uid, AC_UID_LEN) == 0) return (int)i;
    return -1;
}

static const char *result_str(access_result_t r)
{
    switch (r) {
    case ACCESS_GRANTED:               return "GRANTED";
    case ACCESS_DENIED_UNKNOWN_TAG:    return "DENIED (unknown tag)";
    case ACCESS_DENIED_SCHEDULE:       return "DENIED (outside allowed hours)";
    case ACCESS_DENIED_ALREADY_INSIDE: return "DENIED (already inside)";
    case ACCESS_DENIED_NOT_INSIDE:     return "DENIED (not inside)";
    case ACCESS_DENIED_EVACUATION:     return "DENIED (evacuation in progress)";
    case ACCESS_EVAC_SAFE:             return "EVACUATION: marked SAFE";
    default:                           return "?";
    }
}

const char *access_control_loc_str(person_loc_t loc)
{
    switch (loc) {
    case LOC_OUTSIDE: return "OUTSIDE";
    case LOC_LOBBY:   return "LOBBY";
    case LOC_ROOM1:   return "ROOM1";
    case LOC_ROOM2:   return "ROOM2";
    default:          return "?";
    }
}

/* ---- helpers: ALL of these must be called with s_state_mutex held ---- */
static inline bool user_inside(const user_t *u) { return u->loc != LOC_OUTSIDE; }

static void user_set_loc(user_t *u, person_loc_t loc)
{
    if (u->loc == loc) return;
    u->loc   = loc;
    u->since = time(NULL);
}

/* The headcount is always DERIVED from the locations, so it can never drift. */
static void recount_headcount(void)
{
    int n = 0;
    for (size_t i = 0; i < NUM_USERS; i++) if (user_inside(&s_users[i])) n++;
    s_headcount = n;
}

static person_loc_t room_of(reader_t r)
{
    return (r == READER_ROOM1) ? LOC_ROOM1 : LOC_ROOM2;
}

/* Normal operation. `permitted` = building open for this person's role. */
static access_result_t scan_normal(user_t *u, reader_t r, bool permitted)
{
    switch (r) {
    case READER_ENTRY:
        if (!permitted)       return ACCESS_DENIED_SCHEDULE;
        if (user_inside(u))   return ACCESS_DENIED_ALREADY_INSIDE;
        user_set_loc(u, LOC_LOBBY);
        return ACCESS_GRANTED;
    case READER_EXIT:
        if (!permitted)       return ACCESS_DENIED_SCHEDULE;       /* applies to EXIT too */
        if (!user_inside(u))  return ACCESS_DENIED_NOT_INSIDE;
        user_set_loc(u, LOC_OUTSIDE);
        return ACCESS_GRANTED;
    case READER_ROOM1:
    case READER_ROOM2: {
        /* Moving between rooms is allowed at any time once you are inside. */
        if (!user_inside(u))  return ACCESS_DENIED_NOT_INSIDE;
        person_loc_t room = room_of(r);
        user_set_loc(u, (u->loc == room) ? LOC_LOBBY : room);      /* tap = enter / leave the room */
        return ACCESS_GRANTED;
    }
    default:
        return ACCESS_DENIED_UNKNOWN_TAG;
    }
}

/* Evacuation: no schedule, no anti-passback. Reaching the exit = SAFE. */
static access_result_t scan_evacuation(user_t *u, reader_t r)
{
    switch (r) {
    case READER_ENTRY:
        return ACCESS_DENIED_EVACUATION;                           /* nobody goes back in */
    case READER_EXIT:
        user_set_loc(u, LOC_OUTSIDE);
        u->safe = true;
        return ACCESS_EVAC_SAFE;
    case READER_ROOM1:
    case READER_ROOM2: {
        if (u->safe) return ACCESS_DENIED_EVACUATION;              /* already out: no going back */
        person_loc_t room = room_of(r);
        if (user_inside(u)) {
            user_set_loc(u, (u->loc == room) ? LOC_LOBBY : room);
        } else {
            /* Someone nobody knew was inside (forgot to tap in): now they are located. */
            user_set_loc(u, room);
            u->expected = true;
        }
        return ACCESS_GRANTED;
    }
    default:
        return ACCESS_DENIED_UNKNOWN_TAG;
    }
}

static void handle_scan(reader_t r, const uint8_t uid[AC_UID_LEN])
{
    access_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.uid, uid, AC_UID_LEN);
    ev.reader    = r;
    ev.timestamp = time(NULL);

    /* The schedule check locks g_config_mutex, so do it BEFORE taking our own mutex
     * (lock rule: never call another module while holding yours). The whitelist's
     * uid / name / role never change after boot, so reading them here is safe. */
    int  idx       = find_user(uid);
    bool permitted = (idx >= 0) && is_access_permitted_for_role(s_users[idx].role);
    bool open_door = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (idx < 0) {
        ev.result = ACCESS_DENIED_UNKNOWN_TAG;
    } else {
        user_t *u = &s_users[idx];
        strlcpy(ev.name, u->name, sizeof(ev.name));
        ev.result   = s_evacuation ? scan_evacuation(u, r) : scan_normal(u, r, permitted);
        ev.location = u->loc;
        open_door   = (ev.result == ACCESS_GRANTED) && ((int)r < GATE_COUNT);
    }
    recount_headcount();
    ev.headcount = s_headcount;
    xSemaphoreGive(s_state_mutex);

    if (open_door) {
        access_control_open_door((gate_t)r, DOOR_OPEN_MS);
    }

    if (idx < 0) {
        ESP_LOGW(TAG, "UNKNOWN TAG %02X:%02X:%02X:%02X at %s",
                 uid[0], uid[1], uid[2], uid[3], reader_name(r));
    }
    ESP_LOGI(TAG, "[%s] %s %02X:%02X:%02X:%02X -> %s | headcount=%d | location=%s",
             reader_name(r), ev.name[0] ? ev.name : "-",
             uid[0], uid[1], uid[2], uid[3], result_str(ev.result), ev.headcount,
             access_control_loc_str(ev.location));

    /* Hand the event to Member 4's logger (non-blocking: never stall the door) */
    if (s_event_queue) {
        if (xQueueSend(s_event_queue, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "event queue full, event dropped");
        }
    }
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */
void access_control_set_event_queue(QueueHandle_t q)   { s_event_queue = q; }
void access_control_set_spi_mutex(SemaphoreHandle_t m) { s_spi_mutex = m; }

void access_control_set_evacuation(bool active)
{
    bool changed;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    changed = (s_evacuation != active);
    s_evacuation = active;
    if (changed && active) {
        /* Snapshot: everybody inside right now must be accounted for. */
        for (size_t i = 0; i < NUM_USERS; i++) {
            s_users[i].expected = user_inside(&s_users[i]);
            s_users[i].safe     = false;
        }
    } else if (changed && !active) {
        /* End of evacuation: the building is declared EMPTY. (The monitor has
         * already logged who was still missing, before calling us.) */
        for (size_t i = 0; i < NUM_USERS; i++) {
            user_set_loc(&s_users[i], LOC_OUTSIDE);
            s_users[i].expected = false;
            s_users[i].safe     = false;
        }
        recount_headcount();
    }
    if (active) {
        for (int g = 0; g < GATE_COUNT; g++) {
            s_doors[g].open = true;
            s_doors[g].close_at_us = INT64_MAX;
        }
    } else {
        for (int g = 0; g < GATE_COUNT; g++) s_doors[g].close_at_us = 0;
    }
    xSemaphoreGive(s_state_mutex);

    if (!changed) return;
    if (active) {
        for (int g = 0; g < GATE_COUNT; g++) servo_write_angle((gate_t)g, SERVO_ANGLE_OPEN);
        ESP_LOGW(TAG, "evacuation active: both doors held open");
    } else {
        ESP_LOGW(TAG, "evacuation cleared: doors will close, building reset to EMPTY (headcount 0)");
    }
}

int access_control_get_headcount(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int n = s_headcount;
    xSemaphoreGive(s_state_mutex);
    return n;
}

int access_control_list_inside(char names[][AC_NAME_LEN], int max)
{
    int count = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < NUM_USERS && count < max; i++) {
        if (user_inside(&s_users[i])) strlcpy(names[count++], s_users[i].name, AC_NAME_LEN);
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}

int access_control_get_roster(person_status_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < NUM_USERS && n < max; i++) {
        const user_t *u = &s_users[i];
        strlcpy(out[n].name, u->name, AC_NAME_LEN);
        out[n].location = u->loc;
        out[n].since    = u->since;
        out[n].expected = u->expected;
        out[n].safe     = u->safe;
        n++;
    }
    xSemaphoreGive(s_state_mutex);
    return n;
}

void access_control_get_evac_summary(evac_summary_t *s)
{
    memset(s, 0, sizeof(*s));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < NUM_USERS; i++) {
        const user_t *u = &s_users[i];
        if (user_inside(u)) {
            s->inside++;
            if (u->loc == LOC_LOBBY) s->in_lobby++;
            if (u->loc == LOC_ROOM1) s->in_room[0]++;
            if (u->loc == LOC_ROOM2) s->in_room[1]++;
        }
        if (u->expected) {
            s->expected++;
            if (u->safe) s->safe++; else s->missing++;
        }
    }
    xSemaphoreGive(s_state_mutex);
}

esp_err_t access_control_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) return ESP_ERR_NO_MEM;

    /* --- Shared SPI bus. Member 4's SD card will reuse this bus. ---- */
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SPI_MOSI,
        .miso_io_num     = PIN_SPI_MISO,
        .sclk_io_num     = PIN_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,   /* big enough for SD card blocks too */
    };
    esp_err_t err = spi_bus_initialize(RC522_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* --- SPI "devices". ENTRY / EXIT: hardware CS. ROOM1 / ROOM2: ONE shared
     *     software-CS device (see the note at s_soft_cs). --------------------- */
    spi_device_handle_t soft_dev = NULL;
    for (int g = 0; g < READER_COUNT; g++) {
        if (s_soft_cs[g] >= 0) {
            /* CS GPIO: plain output, idle HIGH (= not selected) */
            gpio_config_t cs = {
                .pin_bit_mask = 1ULL << s_soft_cs[g],
                .mode         = GPIO_MODE_OUTPUT,
            };
            gpio_config(&cs);
            gpio_set_level(s_soft_cs[g], 1);

            if (soft_dev == NULL) {
                spi_device_interface_config_t dev = {
                    .clock_speed_hz = 1 * 1000 * 1000,
                    .mode           = 0,
                    .spics_io_num   = -1,              /* software CS */
                    .queue_size     = 3,
                };
                err = spi_bus_add_device(RC522_SPI_HOST, &dev, &soft_dev);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "spi_bus_add_device(room readers, software CS) failed: %s", esp_err_to_name(err));
                    return err;
                }
            }
            s_rc522[g] = soft_dev;
            continue;
        }
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 1 * 1000 * 1000,  /* 1 MHz is safe for RC522 */
            .mode           = 0,
            .spics_io_num   = s_cs_pin[g],
            .queue_size     = 3,
        };
        err = spi_bus_add_device(RC522_SPI_HOST, &dev, &s_rc522[g]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device(%s, CS=%d) failed: %s",
                     reader_name((reader_t)g), s_cs_pin[g], esp_err_to_name(err));
            return err;
        }
    }

    /* --- Shared hardware RESET line for both readers ---------------- */
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_RC522_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level(PIN_RC522_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RC522_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    for (int g = 0; g < READER_COUNT; g++) rc522_chip_init((reader_t)g);

    servos_init();

    ESP_LOGI(TAG, "access control ready (whitelist: %u users, %d readers)", (unsigned)NUM_USERS, READER_COUNT);
    return ESP_OK;
}

/* ================================================================== */
/*  The FreeRTOS task                                                  */
/* ================================================================== */
static bool poll_gate(reader_t g, uint8_t uid[AC_UID_LEN])
{
    bool ok = false;
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);   /* lock shared bus */
    if (picc_request(g)) {
        ESP_LOGI(TAG, "%s: card detected in field", reader_name((reader_t)g));
        if (picc_read_uid(g, uid)) {
            ok = true;
            picc_halt(g);
        } else {
            ESP_LOGW(TAG, "%s: card detected but UID read FAILED", reader_name((reader_t)g));
        }
    }
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);                  /* unlock */
    return ok;
}

void vTaskAccessControl(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "access task running, polling %d readers every %d ms", READER_COUNT, POLL_PERIOD_MS);
    uint8_t last_uid[READER_COUNT][AC_UID_LEN] = {{0}};
    int64_t last_seen_us[READER_COUNT] = {0};

    for (;;) {
        for (int g = 0; g < READER_COUNT; g++) {
            /* A room reader that never answered at boot is simply not wired: skip it
             * so it cannot slow the polling of the real readers. */
            if (g >= GATE_COUNT && !s_reader_ok[g]) continue;
            uint8_t uid[AC_UID_LEN];
            if (poll_gate((reader_t)g, uid)) {
                int64_t now = esp_timer_get_time();
                bool same = memcmp(uid, last_uid[g], AC_UID_LEN) == 0 &&
                            (now - last_seen_us[g]) < SAME_TAG_LOCKOUT_US;
                memcpy(last_uid[g], uid, AC_UID_LEN);
                last_seen_us[g] = now;
                if (!same) handle_scan((reader_t)g, uid);   /* debounce */
            }
        }
        doors_update();
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}