#ifndef PINS_H
#define PINS_H

/* shared SPI3_HOST bus for RFID readers and SD card */
#define PIN_SPI_SCK         18
#define PIN_SPI_MISO        19
#define PIN_SPI_MOSI        23
#define PIN_RFID_RST        15
#define PIN_RFID_CS_ENTRY    5
#define PIN_RFID_CS_EXIT     4
#define PIN_RFID_CS_ROOM1    0   /* lecture room 1 door reader (boot pin: must be HIGH at reset) */
#define PIN_RFID_CS_ROOM2   12   /* lecture room 2 door reader (strapping pin: see README)       */
#define PIN_SD_CS            2   /* SD card chip select on the shared bus */

/* door servos use LEDC timer 0, channels 0-1 */
#define PIN_SERVO_ENTRY     27
#define PIN_SERVO_EXIT      33

/* room lighting uses LEDC timer 1, channels 2-3 */
#define PIN_PIR_ROOM1       13
#define PIN_PIR_ROOM2       14
#define PIN_LED_ROOM1       25
#define PIN_LED_ROOM2       26
#define PIN_POT_DIMMER      34   /* wired, currently unused */

/* alerts and evacuation */
#define PIN_ALERT_LED       16   /* red LED, intrusion alert */
#define PIN_BUZZER          17
#define PIN_EVAC_BUTTON     32   /* pushbutton to GND, internal pull-up */

/* I2C LCD with PCF8574 backpack */
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22

/* USB serial console on UART0 */
#define PIN_UART_TX          1
#define PIN_UART_RX          3

/* GPIO 0, 2, 12 and 15 are strapping pins (real hardware only; Wokwi ignores this).
 * No output-capable pins are left now. Only input-only GPIO 35, 36, 39 remain free. */
#endif /* PINS_H */