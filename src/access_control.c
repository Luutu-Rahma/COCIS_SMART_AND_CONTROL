/*
 * access_control.c  --  MEMBER 1: Main Entrance & RFID Access Control
 *
 * What this file does:
 *   1. Talks to TWO MFRC522 (RC522) RFID readers over ONE shared SPI bus.
 *   2. Reads the 4-byte UID of a tag, checks it against a whitelist.
 *   3. Opens the correct door servo using PWM (LEDC) for a few seconds.
 *   4. Keeps the live headcount (+1 on entry, -1 on exit) and who is inside.
 *   5. Emits an access_event_t to a queue so Member 4 can log it to the SD card.
 *
 * Pin map (matches the Wokwi diagram):
 *   SPI bus (VSPI):  SCK=18  MISO=19  MOSI=23
 *   Entry reader:    SDA/CS=5        Exit reader: SDA/CS=4
 *   Shared RST:      15
 *   Entry servo PWM: 27              Exit servo PWM: 33
 */
#include "access_control.h"

#include <string.h>
#include <stdio.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define PIN_RC522_RST    15
#define PIN_SERVO_ENTRY  27
#define PIN_SERVO_EXIT   33

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
static spi_device_handle_t s_rc522[GATE_COUNT];

static SemaphoreHandle_t s_spi_mutex   = NULL;  /* from Member 3 (optional) */
static QueueHandle_t     s_event_queue = NULL;  /* from Member 3 (optional) */
static SemaphoreHandle_t s_state_mutex = NULL;  /* OURS: guards state below */

/* --- Whitelist ---------------------------------------------------- */
typedef struct {
    uint8_t     uid[AC_UID_LEN];
    const char *name;
    bool        inside;           /* anti-passback flag */
} user_t;

/*
 * Wokwi's built-in cards (click the RFID reader -> pick a card):
 *   Blue 01:02:03:04   Green 11:22:33:44   Yellow 55:66:77:88
 *   Red  AA:BB:CC:DD   Key fob C0:FF:EE:99
 *   (the grey "NFC Tag" has a 7-byte UID and is NOT supported by this driver)
 * Red is deliberately left OUT so you can test "unknown tag".
 */
static user_t s_users[] = {
    { {0x01, 0x02, 0x03, 0x04}, "Student A",  false },   /* Blue   */
    { {0x11, 0x22, 0x33, 0x44}, "Student B",  false },   /* Green  */
    { {0x55, 0x66, 0x77, 0x88}, "Lecturer",   false },   /* Yellow */
    { {0xC0, 0xFF, 0xEE, 0x99}, "Security",   false },   /* Key fob */
};
#define NUM_USERS (sizeof(s_users) / sizeof(s_users[0]))

static int s_headcount = 0;

/* --- Doors -------------------------------------------------------- */
typedef struct {
    bool    open;
    int64_t close_at_us;
} door_t;
static door_t s_doors[GATE_COUNT];

static const int           s_servo_pin[GATE_COUNT] = { PIN_SERVO_ENTRY, PIN_SERVO_EXIT };
static const ledc_channel_t s_servo_ch[GATE_COUNT] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };
static const int           s_cs_pin[GATE_COUNT]    = { PIN_CS_ENTRY, PIN_CS_EXIT };

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
static void rc522_write(gate_t g, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_transmit(s_rc522[g], &t);
}

static uint8_t rc522_read(gate_t g, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(s_rc522[g], &t);
    return rx[1];
}

static void rc522_set_bits(gate_t g, uint8_t reg, uint8_t mask)
{
    rc522_write(g, reg, rc522_read(g, reg) | mask);
}

static void rc522_clear_bits(gate_t g, uint8_t reg, uint8_t mask)
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

static void rc522_write_fifo(gate_t g, const uint8_t *data, uint8_t len)
{
    uint8_t tx[FIFO_MAX + 1];
    if (len > FIFO_MAX) len = FIFO_MAX;
    tx[0] = (uint8_t)((REG_FIFO_DATA << 1) & 0x7E);
    memcpy(&tx[1], data, len);
    spi_transaction_t t = { .length = (size_t)(len + 1) * 8, .tx_buffer = tx };
    spi_device_transmit(s_rc522[g], &t);
}

static void rc522_read_fifo(gate_t g, uint8_t *buf, uint8_t n)
{
    uint8_t tx[FIFO_MAX + 1], rx[FIFO_MAX + 1];
    if (n == 0) return;
    if (n > FIFO_MAX) n = FIFO_MAX;
    uint8_t addr = (uint8_t)(((REG_FIFO_DATA << 1) & 0x7E) | 0x80);
    for (uint8_t i = 0; i < n; i++) tx[i] = addr;
    tx[n] = 0x00;
    memset(rx, 0, sizeof(rx));
    spi_transaction_t t = { .length = (size_t)(n + 1) * 8, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(s_rc522[g], &t);
    memcpy(buf, &rx[1], n);
}

/* ================================================================== */
/*  LAYER 2: talking to a card through the RC522                       */
/* ================================================================== */

/* Ask the RC522's hardware to compute the ISO14443 CRC_A of a buffer. */
static bool rc522_calc_crc(gate_t g, const uint8_t *data, uint8_t len, uint8_t out[2])
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
static bool rc522_transceive(gate_t g, const uint8_t *tx, uint8_t tx_len,
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
static bool picc_request(gate_t g)
{
    rc522_clear_bits(g, REG_COLL, 0x80);
    uint8_t cmd = PICC_REQA, atqa[2], len = sizeof(atqa);
    return rc522_transceive(g, &cmd, 1, atqa, &len, 7) && len == 2;
}

/* Step 2+3: anti-collision (card sends its UID) then SELECT it. */
static bool picc_read_uid(gate_t g, uint8_t uid[AC_UID_LEN])
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
static void picc_halt(gate_t g)
{
    uint8_t buf[4] = { PICC_HLTA, 0x00 };
    if (!rc522_calc_crc(g, buf, 2, &buf[2])) return;
    rc522_transceive(g, buf, 4, NULL, NULL, 0);   /* card never answers (timeout is normal) */
}

static void rc522_chip_init(gate_t g)
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
    ESP_LOGI(TAG, "%s reader VersionReg = 0x%02X %s",
             g == GATE_ENTRY ? "ENTRY" : "EXIT", ver,
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
        if (s_doors[g].open && now >= s_doors[g].close_at_us) {
            s_doors[g].open = false;
            close_now = true;
        }
        xSemaphoreGive(s_state_mutex);
        if (close_now) {
            servo_write_angle((gate_t)g, SERVO_ANGLE_CLOSED);
            ESP_LOGI(TAG, "%s DOOR CLOSED", g == GATE_ENTRY ? "ENTRY" : "EXIT");
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
    default:                           return "?";
    }
}

static void handle_scan(gate_t g, const uint8_t uid[AC_UID_LEN])
{
    access_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.uid, uid, AC_UID_LEN);
    ev.gate      = g;
    ev.timestamp = time(NULL);

    /* Ask Member 4 BEFORE taking our own mutex (avoid nested locking). */
    bool open_hours = is_access_permitted();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int idx = find_user(uid);
    if (idx < 0) {
        ev.result = ACCESS_DENIED_UNKNOWN_TAG;
    } else {
        strlcpy(ev.name, s_users[idx].name, sizeof(ev.name));
        if (!open_hours) {
            ev.result = ACCESS_DENIED_SCHEDULE;          /* applies to EXIT too */
        } else if (g == GATE_ENTRY && s_users[idx].inside) {
            ev.result = ACCESS_DENIED_ALREADY_INSIDE;
        } else if (g == GATE_EXIT && !s_users[idx].inside) {
            ev.result = ACCESS_DENIED_NOT_INSIDE;
        } else {
            ev.result = ACCESS_GRANTED;
            s_users[idx].inside = (g == GATE_ENTRY);
            s_headcount += (g == GATE_ENTRY) ? 1 : -1;
            if (s_headcount < 0) s_headcount = 0;        /* safety net */
        }
    }
    ev.headcount = s_headcount;
    xSemaphoreGive(s_state_mutex);

    if (ev.result == ACCESS_GRANTED) {
        access_control_open_door(g, DOOR_OPEN_MS);
    }

    if (idx < 0) {
        ESP_LOGW(TAG, "UNKNOWN TAG %02X:%02X:%02X:%02X at %s",
                 uid[0], uid[1], uid[2], uid[3], g == GATE_ENTRY ? "ENTRY" : "EXIT");
    }
    ESP_LOGI(TAG, "[%s] %s %02X:%02X:%02X:%02X -> %s | headcount=%d",
             g == GATE_ENTRY ? "ENTRY" : "EXIT", ev.name[0] ? ev.name : "-",
             uid[0], uid[1], uid[2], uid[3], result_str(ev.result), ev.headcount);

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
        if (s_users[i].inside) strlcpy(names[count++], s_users[i].name, AC_NAME_LEN);
    }
    xSemaphoreGive(s_state_mutex);
    return count;
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

    /* --- One SPI "device" per reader: same bus, different CS pin ---- */
    for (int g = 0; g < GATE_COUNT; g++) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 1 * 1000 * 1000,  /* 1 MHz is safe for RC522 */
            .mode           = 0,
            .spics_io_num   = s_cs_pin[g],
            .queue_size     = 3,
        };
        err = spi_bus_add_device(RC522_SPI_HOST, &dev, &s_rc522[g]);
        if (err != ESP_OK) return err;
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

    for (int g = 0; g < GATE_COUNT; g++) rc522_chip_init((gate_t)g);

    servos_init();

    ESP_LOGI(TAG, "access control ready (whitelist: %u users)", (unsigned)NUM_USERS);
    return ESP_OK;
}

/* ================================================================== */
/*  The FreeRTOS task                                                  */
/* ================================================================== */
static bool poll_gate(gate_t g, uint8_t uid[AC_UID_LEN])
{
    bool ok = false;
    if (s_spi_mutex) xSemaphoreTake(s_spi_mutex, portMAX_DELAY);   /* lock shared bus */
    if (picc_request(g)) {
        ESP_LOGI(TAG, "%s: card detected in field", g == GATE_ENTRY ? "ENTRY" : "EXIT");
        if (picc_read_uid(g, uid)) {
            ok = true;
            picc_halt(g);
        } else {
            ESP_LOGW(TAG, "%s: card detected but UID read FAILED", g == GATE_ENTRY ? "ENTRY" : "EXIT");
        }
    }
    if (s_spi_mutex) xSemaphoreGive(s_spi_mutex);                  /* unlock */
    return ok;
}

void vTaskAccessControl(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "access task running, polling both readers every %d ms", POLL_PERIOD_MS);
    uint8_t last_uid[GATE_COUNT][AC_UID_LEN] = {{0}};
    int64_t last_seen_us[GATE_COUNT] = {0};

    for (;;) {
        for (int g = 0; g < GATE_COUNT; g++) {
            uint8_t uid[AC_UID_LEN];
            if (poll_gate((gate_t)g, uid)) {
                int64_t now = esp_timer_get_time();
                bool same = memcmp(uid, last_uid[g], AC_UID_LEN) == 0 &&
                            (now - last_seen_us[g]) < SAME_TAG_LOCKOUT_US;
                memcpy(last_uid[g], uid, AC_UID_LEN);
                last_seen_us[g] = now;
                if (!same) handle_scan((gate_t)g, uid);   /* debounce */
            }
        }
        doors_update();
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}