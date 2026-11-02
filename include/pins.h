/*
 * pins.h -- the ONE place every GPIO is written down (owner: Member 5; corrected by
 * Member 3 so the SD card shares the RFID SPI bus). Plain ints so the macros work in
 * gpio_config(), ledc, spi and i2c structs.
 */
#ifndef PINS_H
#define PINS_H

/* Shared SPI3_HOST (VSPI) bus: RFID readers AND SD card */
#define PIN_SPI_SCK         18
#define PIN_SPI_MISO        19
#define PIN_SPI_MOSI        23
#define PIN_RFID_RST        15
#define PIN_RFID_CS_ENTRY    5
#define PIN_RFID_CS_EXIT     4
#define PIN_SD_CS            2   /* SD card chip select on the shared bus */

/* Door servos (LEDC timer 0, channels 0-1) */
#define PIN_SERVO_ENTRY     27
#define PIN_SERVO_EXIT      33

/* Lecture rooms (LEDC timer 1, channels 2-3) */
#define PIN_PIR_ROOM1       13
#define PIN_PIR_ROOM2       14
#define PIN_LED_ROOM1       25
#define PIN_LED_ROOM2       26
#define PIN_POT_DIMMER      34   /* wired, currently unused */

/* Alerts and emergency */
#define PIN_ALERT_LED       16   /* red LED, intrusion alert */
#define PIN_BUZZER          17
#define PIN_EVAC_BUTTON     32   /* pushbutton to GND, internal pull-up */

/* I2C LCD (PCF8574 backpack) */
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22

/* USB serial console (UART0) */
#define PIN_UART_TX          1
#define PIN_UART_RX          3

/* GPIO 2 and 15 are strapping pins (real hardware only; Wokwi ignores this).
 * Free output-capable pins left: 0 (boot button) and 12 (strapping). */
#endif /* PINS_H */
