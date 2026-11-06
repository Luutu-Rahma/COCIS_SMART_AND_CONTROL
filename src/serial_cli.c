#include "serial_cli.h"
#include "pins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strcasecmp */
#include <time.h>
#include <stdint.h>

#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "access_control.h"   /* Member 1: headcount, open door      */
#include "schedule.h"         /* Member 4: opening hours, clock      */
#include "storage_sync.h"     /* Member 3: locked SD log access      */
#include "system_sync.h"      /* Member 3: evacuation, system log    */
#include "lighting_control.h" /* Member 2: room state                */

static const char *TAG = "CLI";

#define CLI_UART          UART_NUM_0     /* the USB serial console        */
#define CLI_LINE_MAX      64
#define CLI_MAX_WORDS     5

#define ADMIN_PASSWORD    "cocis2026"    /* change this before the demo    */
#define ADMIN_SESSION_US  (120LL * 1000000)  /* login lasts 2 minutes       */
#define MAX_FAILED_LOGINS 3
#define LOCKOUT_US        (30LL * 1000000)   /* 30 s lock after 3 failures  */

#define LCD_ADDR          0x27           /* PCF8574 backpack address        */
#define LCD_COLS          20

/* Optional hooks for lighting controls. */
/* Member 2: room 0 = all, 1 or 2; mode 0 = auto, 1 = on, 2 = off. */
extern void lighting_set_override(int room, int mode) __attribute__((weak));
/* Member 2: optional energy-saved duration hook. */
extern uint32_t lighting_get_energy_saved_s(void) __attribute__((weak));

static char s_line[CLI_LINE_MAX];
static int  s_len = 0;

static int64_t s_admin_until_us  = 0;
static int64_t s_locked_until_us = 0;
static int     s_failed_logins   = 0;

static int    s_last_count = 0;
static int    s_peak       = 0;
static time_t s_peak_time  = 0;
static int    s_entries    = 0;
static int    s_exits      = 0;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_lcd     = NULL;
static bool                    s_lcd_ok  = false;

/* Round up to at least one FreeRTOS tick. */
static void delay_ms(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS + 1);
}

static bool is_admin(void)
{
    return esp_timer_get_time() < s_admin_until_us;
}

/* Require an active admin session. */
static bool require_admin(void)
{
    if (is_admin()) return true;
    printf("Admin only. Use: login <password>\n");
    return false;
}

/* PCF8574 bit mapping for the HD44780 LCD. */
/*
 * The PCF8574 has 8 output pins wired to the LCD like this:
 *   bit0 = RS (0 command / 1 data)   bit1 = RW (always 0)
 *   bit2 = EN (pulse to latch)       bit3 = backlight
 *   bit4..7 = D4..D7 (the LCD runs in 4-bit mode, so each byte is sent
 *             as two 4-bit halves)
 */
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08

static void lcd_expander_write(uint8_t b)
{
    if (s_lcd) i2c_master_transmit(s_lcd, &b, 1, 50);
}

/* Write one nibble and pulse EN. */
static void lcd_write_nibble(uint8_t nibble, uint8_t rs)
{
    uint8_t base = (uint8_t)((nibble << 4) | LCD_BL | rs);
    lcd_expander_write(base | LCD_EN);
    lcd_expander_write(base & (uint8_t)~LCD_EN);
}

static void lcd_send(uint8_t value, uint8_t rs)
{
    lcd_write_nibble(value >> 4, rs);      /* high half first */
    lcd_write_nibble(value & 0x0F, rs);
}

/* Write one padded LCD row. */
static void lcd_row(int row, const char *text)
{
    static const uint8_t row_start[4] = { 0x00, 0x40, 0x14, 0x54 };
    lcd_send((uint8_t)(0x80 | row_start[row]), 0);   /* move the cursor */
    for (int i = 0; i < LCD_COLS; i++) {
        char c = (text[i] != '\0') ? text[i] : ' ';
        lcd_send((uint8_t)c, LCD_RS);
        if (text[i] == '\0') {                       /* pad the rest */
            for (i++; i < LCD_COLS; i++) lcd_send(' ', LCD_RS);
            break;
        }
    }
}

static void lcd_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus failed; LCD disabled");
        return;
    }
    /* LCD is optional; continue if it is absent. */
    if (i2c_master_probe(s_i2c_bus, LCD_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no LCD at 0x%02X; LCD disabled", LCD_ADDR);
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LCD_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_lcd) != ESP_OK) return;

    /* HD44780 4-bit initialization sequence */
    delay_ms(50);
    lcd_write_nibble(0x03, 0); delay_ms(5);
    lcd_write_nibble(0x03, 0); delay_ms(5);
    lcd_write_nibble(0x03, 0); delay_ms(5);
    lcd_write_nibble(0x02, 0); delay_ms(5);   /* switch to 4-bit mode */
    lcd_send(0x28, 0);                        /* 4-bit, 2+ lines, 5x8 font */
    lcd_send(0x0C, 0);                        /* display on, cursor off    */
    lcd_send(0x06, 0);                        /* cursor moves right        */
    lcd_send(0x01, 0); delay_ms(5);           /* clear screen              */

    s_lcd_ok = true;
    lcd_row(0, "CoCIS Smart System");
    lcd_row(1, "Starting...");
    ESP_LOGI(TAG, "LCD ready at 0x%02X", LCD_ADDR);
}

/* Refresh live LCD diagnostics. */
static void lcd_refresh(void)
{
    if (!s_lcd_ok) return;
    if (system_evacuation_active()) {          /* emergency screen replaces the normal one */
        evac_report_t rep;
        char eb[64];
        system_evac_get_report(&rep);
        lcd_row(0, "!! EVACUATION !!");
        snprintf(eb, sizeof(eb), "Inside:%d Safe:%d/%d", rep.people.inside, rep.people.safe, rep.people.expected);
        lcd_row(1, eb);
        snprintf(eb, sizeof(eb), "R1:%d R2:%d Lobby:%d", rep.people.in_room[0], rep.people.in_room[1], rep.people.in_lobby);
        lcd_row(2, eb);
        if (rep.all_clear) {
            lcd_row(3, "ALL CLEAR-BLDG EMPTY");
        } else if (rep.unregistered[0] || rep.unregistered[1]) {
            snprintf(eb, sizeof(eb), "UNTAGGED MOTION R%d", rep.unregistered[0] ? 1 : 2);
            lcd_row(3, eb);
        } else {
            snprintf(eb, sizeof(eb), "MISSING:%d SEARCH!", rep.people.missing);
            lcd_row(3, eb);
        }
        return;
    }
    char buf[32];
    lcd_row(0, "CoCIS Smart System");
    snprintf(buf, sizeof(buf), "Inside:%d  Peak:%d", s_last_count, s_peak);
    lcd_row(1, buf);
    snprintf(buf, sizeof(buf), "Building: %s",
             is_access_permitted() ? "OPEN" : "CLOSED");
    lcd_row(2, buf);
    snprintf(buf, sizeof(buf), "In:%d Out:%d %s", s_entries, s_exits,
             is_admin() ? "ADMIN" : "");
    lcd_row(3, buf);
}

/* Analytics sampling: simultaneous entry and exit events may cancel out. */
static void sample_stats(void)
{
    int n = access_control_get_headcount();
    if (n > s_last_count) s_entries += n - s_last_count;
    if (n < s_last_count) s_exits   += s_last_count - n;
    s_last_count = n;
    if (n > s_peak) {
        s_peak = n;
        time(&s_peak_time);
    }
}

static void cmd_help(void)
{
    printf("Commands:\n"
           "  help                              this list\n"
           "  status                            time, hours, headcount, who is inside\n"
           "  roster                            who is where; during evacuation: safe / missing\n"
           "  logs                              print the SD card access log\n"
           "  analytics                         peak occupancy, entries/exits, energy\n"
           "  login <password>                  become admin for 2 minutes\n"
           "  logout                            end the admin session\n"
           "Admin only:\n"
           "  set-schedule <open_h> <close_h>   e.g. set-schedule 7 22\n"
           "  settime HH:MM                     set the clock (no battery clock)\n"
           "  override-lights <1|2|all> <on|off|auto>\n"
           "  open-door <entry|exit>            open a door for 3 seconds\n"
           "  evacuate <on|off>                 emergency: doors open, lights full; 'off' empties the building\n");
}

static void cmd_status(void)
{
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    printf("Time:       %s\n", stamp);
    int open_h = 0, close_h = 0;
    schedule_get_hours(&open_h, &close_h);
    printf("Hours:      %02d:00 - %02d:00 (%s now)\n", open_h, close_h,
           is_access_permitted() ? "OPEN" : "CLOSED");
    printf("Headcount:  %d\n", access_control_get_headcount());

    person_status_t people[16];
    int np = access_control_get_roster(people, 16);
    int shown = 0;
    printf("Inside:     ");
    for (int i = 0; i < np; i++) {
        if (people[i].location == LOC_OUTSIDE) continue;
        printf("%s%s (%s)", shown ? ", " : "", people[i].name, access_control_loc_str(people[i].location));
        shown++;
    }
    if (shown == 0) printf("nobody");
    printf("\n");
    printf("Uptime:     %lld s\n", (long long)(esp_timer_get_time() / 1000000));
    printf("Admin:      %s\n", is_admin() ? "logged in" : "not logged in");
    for (int room = 1; room <= 2; room++) {
        RoomState_t st = lighting_get_room_state(room);
        printf("Room %d:     %s, motion %s, light %u%%\n", room,
               st == ROOM_OCCUPIED ? "OCCUPIED" : (st == ROOM_STANDBY ? "STANDBY" : "VACANT"),
               lighting_get_motion(room) ? "yes" : "no",
               (unsigned)lighting_get_brightness_pct(room));
    }
    printf("Evacuation: %s\n", system_evacuation_active() ? "ACTIVE" : "off");
}

/* Who is where. Read-only, so no admin login needed. */
static void cmd_roster(void)
{
    person_status_t p[16];
    int n = access_control_get_roster(p, 16);
    evac_report_t rep;
    system_evac_get_report(&rep);
    bool evac = system_evacuation_active();

    printf("=== BUILDING ROSTER%s ===\n", evac ? " - EVACUATION ACTIVE" : "");
    printf("Inside: %d  (lobby %d, room 1: %d, room 2: %d)\n",
           rep.people.inside, rep.people.in_lobby, rep.people.in_room[0], rep.people.in_room[1]);
    if (evac) {
        printf("Safe: %d of %d   Missing / still inside: %d\n",
               rep.people.safe, rep.people.expected, rep.people.missing);
    }
    printf("%-16s %-8s %-6s %s\n", "Name", "Where", "Since", evac ? "Status" : "");
    for (int i = 0; i < n; i++) {
        char hm[32] = "--:--";
        if (p[i].since != 0) {
            struct tm tm;
            localtime_r(&p[i].since, &tm);
            snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
        }
        const char *status = "";
        if (evac) {
            if (p[i].safe)          status = "SAFE (outside)";
            else if (p[i].expected) status = "STILL INSIDE - RESCUE";
            else                    status = "not in building";
        }
        printf("%-16s %-8s %-6s %s\n", p[i].name, access_control_loc_str(p[i].location), hm, status);
    }
    for (int i = 0; i < 2; i++) {
        printf("Room %d PIR: %s%s\n", i + 1, rep.motion[i] ? "motion" : "quiet",
               rep.unregistered[i] ? "  <-- NOBODY TAGGED HERE: possible untagged person!" : "");
    }
    if (evac && rep.all_clear) printf("ALL CLEAR: the building is empty.\n");
}

static void cmd_analytics(void)
{
    char pk[16] = "-";
    if (s_peak_time != 0) {
        struct tm tm;
        localtime_r(&s_peak_time, &tm);
        strftime(pk, sizeof(pk), "%H:%M", &tm);
    }
    printf("Current headcount: %d\n", s_last_count);
    printf("Peak occupancy:    %d (at %s)\n", s_peak, pk);
    printf("Entries / exits:   %d / %d\n", s_entries, s_exits);
    if (lighting_get_energy_saved_s) {
        printf("Energy saved:      %lu s of dimmed/off lighting\n",
               (unsigned long)lighting_get_energy_saved_s());
    } else {
        printf("Energy saved:      not available (lighting module has no counter yet)\n");
    }
    printf("(numbers count from boot)\n");
}

static void cmd_login(const char *pw)
{
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us) {
        printf("Locked. Try again in %lld s.\n",
               (long long)((s_locked_until_us - now) / 1000000 + 1));
        return;
    }
    if (pw && strcmp(pw, ADMIN_PASSWORD) == 0) {
        s_failed_logins  = 0;
        s_admin_until_us = now + ADMIN_SESSION_US;
        printf("Admin session open for 2 minutes.\n");
        return;
    }
    s_failed_logins++;
    printf("Wrong password (%d/%d).\n", s_failed_logins, MAX_FAILED_LOGINS);
    system_log_post(SYSLOG_AUTH_FAIL, "Failed admin login %d/%d on serial", s_failed_logins, MAX_FAILED_LOGINS);
    if (s_failed_logins >= MAX_FAILED_LOGINS) {
        s_locked_until_us = now + LOCKOUT_US;
        s_failed_logins   = 0;
        printf("Too many attempts. Locked for 30 s.\n");
    }
}

static void cmd_set_schedule(const char *a, const char *b)
{
    if (!require_admin()) return;
    if (!a || !b) { printf("Usage: set-schedule <open_h> <close_h>\n"); return; }
    int open_h = atoi(a), close_h = atoi(b);
    if (open_h < 0 || close_h > 24 || open_h >= close_h) {
        printf("Invalid hours. Need 0 <= open < close <= 24.\n");
        return;
    }
    schedule_set_hours(open_h, close_h);
    printf("Hours set to %02d:00 - %02d:00.\n", open_h, close_h);
}

static void cmd_settime(const char *hhmm)
{
    if (!require_admin()) return;
    int h = -1, m = -1;
    if (!hhmm || sscanf(hhmm, "%d:%d", &h, &m) != 2 ||
        h < 0 || h > 23 || m < 0 || m > 59) {
        printf("Usage: settime HH:MM   (24-hour, e.g. settime 08:30)\n");
        return;
    }
    schedule_set_time(h, m);
    printf("Clock set to %02d:%02d.\n", h, m);
}

static void cmd_override_lights(const char *room_s, const char *mode_s)
{
    if (!require_admin()) return;
    if (!room_s || !mode_s) {
        printf("Usage: override-lights <1|2|all> <on|off|auto>\n");
        return;
    }
    int room = -1, mode = -1;
    if (strcasecmp(room_s, "all") == 0) room = 0;
    else if (strcmp(room_s, "1") == 0)  room = 1;
    else if (strcmp(room_s, "2") == 0)  room = 2;
    if (strcasecmp(mode_s, "auto") == 0)     mode = 0;
    else if (strcasecmp(mode_s, "on") == 0)  mode = 1;
    else if (strcasecmp(mode_s, "off") == 0) mode = 2;
    if (room < 0 || mode < 0) {
        printf("Usage: override-lights <1|2|all> <on|off|auto>\n");
        return;
    }
    if (!lighting_set_override) {
        printf("Not available yet: Member 2 must add lighting_set_override().\n");
        return;
    }
    lighting_set_override(room, mode);
    printf("Lights: room %s -> %s\n", room_s, mode_s);
}

static void cmd_open_door(const char *which)
{
    if (!require_admin()) return;
    if (which && strcasecmp(which, "entry") == 0)      access_control_open_door(GATE_ENTRY, 3000);
    else if (which && strcasecmp(which, "exit") == 0)  access_control_open_door(GATE_EXIT, 3000);
    else { printf("Usage: open-door <entry|exit>\n"); return; }
    system_log_post(SYSLOG_ADMIN_ACTION, "Admin opened %s door from CLI", which);
    printf("Opening %s door.\n", which);
}

static void cmd_evacuate(const char *mode)
{
    if (!require_admin()) return;
    if (mode && strcasecmp(mode, "on") == 0)       system_evacuation_set(true, "cli");
    else if (mode && strcasecmp(mode, "off") == 0) {
        if (system_evacuation_active()) {
            evac_report_t rep;
            system_evac_get_report(&rep);
            if (rep.people.inside > 0 || rep.people.missing > 0 || !rep.all_clear)
                printf("WARNING: %d people still inside / %d not accounted for. Clearing anyway.\n",
                       rep.people.inside, rep.people.missing);
        }
        system_evacuation_set(false, "cli");
    }
    else { printf("Usage: evacuate <on|off>\n"); return; }
    printf("Evacuation %s.\n", mode);
    if (strcasecmp(mode, "off") == 0) printf("Building reset to EMPTY (headcount 0).\n");
}

void parse_cli_command(char *line)
{
    /* split the command in place */
    char *w[CLI_MAX_WORDS] = { 0 };
    int   n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save);
         tok && n < CLI_MAX_WORDS;
         tok = strtok_r(NULL, " \t", &save)) {
        w[n++] = tok;
    }
    if (n == 0) return;

    if      (strcasecmp(w[0], "help") == 0)            cmd_help();
    else if (strcasecmp(w[0], "status") == 0)          cmd_status();
    else if (strcasecmp(w[0], "roster") == 0)          cmd_roster();
    else if (strcasecmp(w[0], "logs") == 0)            storage_print_log();
    else if (strcasecmp(w[0], "analytics") == 0)       cmd_analytics();
    else if (strcasecmp(w[0], "login") == 0)           cmd_login(w[1]);
    else if (strcasecmp(w[0], "logout") == 0) { s_admin_until_us = 0; printf("Logged out.\n"); }
    else if (strcasecmp(w[0], "set-schedule") == 0)    cmd_set_schedule(w[1], w[2]);
    else if (strcasecmp(w[0], "settime") == 0)         cmd_settime(w[1]);
    else if (strcasecmp(w[0], "override-lights") == 0) cmd_override_lights(w[1], w[2]);
    else if (strcasecmp(w[0], "open-door") == 0)       cmd_open_door(w[1]);
    else if (strcasecmp(w[0], "evacuate") == 0)        cmd_evacuate(w[1]);
    else printf("Unknown command '%s'. Type help.\n", w[0]);
}

static void prompt(void)
{
    printf("cocis> ");
    fflush(stdout);
}

static void handle_char(uint8_t ch)
{
    if (ch == '\r' || ch == '\n') {
        if (s_len == 0) return;          /* ignore the 2nd half of \r\n */
        s_line[s_len] = '\0';
        printf("\n");
        parse_cli_command(s_line);
        s_len = 0;
        prompt();
    } else if (ch == 0x08 || ch == 0x7F) {          /* backspace */
        if (s_len > 0) { s_len--; printf("\b \b"); fflush(stdout); }
    } else if (ch >= 0x20 && ch < 0x7F && s_len < CLI_LINE_MAX - 1) {
        s_line[s_len++] = (char)ch;
        printf("%c", ch);                    /* echo what was typed */
        fflush(stdout);
    }
}

esp_err_t serial_cli_init(void)
{
    /* keep the RX buffer larger than the 128-byte hardware FIFO */
    esp_err_t err = uart_driver_install(CLI_UART, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    lcd_init();    /* optional: never fails the whole init */
    ESP_LOGI(TAG, "serial CLI ready");
    return ESP_OK;
}

void vTaskSerialCLI(void *pvParameters)
{
    (void)pvParameters;
    printf("\n=== CoCIS Smart System CLI ===  type help\n");
    prompt();

    int64_t next_sample_us = 0;
    int64_t next_lcd_us    = 0;
    bool evac_prev = false;

    for (;;) {
    /* keep the task responsive while waiting for input */
        uint8_t ch;
        while (uart_read_bytes(CLI_UART, &ch, 1, pdMS_TO_TICKS(50)) == 1) {
            handle_char(ch);
        }

        int64_t now = esp_timer_get_time();
        /* refresh the alert immediately when evacuation state changes */
        bool evac = system_evacuation_active();
        if (evac != evac_prev) {
            evac_prev = evac;
            printf("\n\n*** EMERGENCY: EVACUATION %s ***\n\n",
                   evac ? "ACTIVE - doors open, lights full, leave the building"
                        : "CLEARED - normal operation resumed");
            lcd_refresh();
            next_lcd_us = now + 1000000;
            prompt();
            if (s_len > 0) { printf("%.*s", (int)s_len, s_line); fflush(stdout); }   /* restore half-typed text */
        }
        if (now >= next_sample_us) { sample_stats(); next_sample_us = now + 500000; }
        if (now >= next_lcd_us)    { lcd_refresh();  next_lcd_us    = now + 1000000; }
    }
}