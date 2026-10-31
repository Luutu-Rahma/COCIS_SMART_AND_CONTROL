/*
 * pins.h  --  MEMBER 5: the ONE place every GPIO number is written down.
 *
 * WHY THIS FILE EXISTS
 *   Five people wire one ESP32. If two modules use the same GPIO, the
 *   hardware silently misbehaves. Keeping every pin in one file lets the
 *   whole team see conflicts at a glance. Other modules should
 *   #include "pins.h" instead of hard-coding numbers.
 *
 * HOW IT WAS BUILT
 *   Pins marked [as-is] are copied from code that already exists in the
 *   repo, so nobody has to change them. Pins marked [new] are proposals
 *   for things that are not wired yet.
 *
 * Plain ints are used (not GPIO_NUM_x) so the same macro works in
 * gpio_config(), ledc, spi and i2c structs alike.
 */
#ifndef PINS_H
#define PINS_H

/* ================================================================== */
/*  Member 1: RFID readers (shared SPI bus, host SPI3_HOST / VSPI)      */
/* ================================================================== */
#define PIN_RFID_SCK        18   /* [as-is] shared clock                */
#define PIN_RFID_MISO       19   /* [as-is] shared data from readers    */
#define PIN_RFID_MOSI       23   /* [as-is] shared data to readers      */
#define PIN_RFID_CS_ENTRY    5   /* [as-is] chip select, entry reader   */
#define PIN_RFID_CS_EXIT     4   /* [as-is] chip select, exit reader    */
#define PIN_RFID_RST        15   /* [as-is] shared reset line           */

/* Door servos (PWM) */
#define PIN_SERVO_ENTRY     27   /* [as-is] entry door servo            */
#define PIN_SERVO_EXIT      33   /* [as-is] exit door servo             */

/* ================================================================== */
/*  Member 2: lecture room lighting                                     */
/* ================================================================== */
#define PIN_PIR_ROOM1       13   /* [as-is] motion sensor, room 1       */
#define PIN_PIR_ROOM2       14   /* [as-is] motion sensor, room 2       */
#define PIN_LED_ROOM1       25   /* [as-is] room 1 light (PWM)          */
#define PIN_LED_ROOM2       26   /* [as-is] room 2 light (PWM)          */
#define PIN_POT_DIMMER      34   /* [as-is] slide pot, input-only pin   */

/* ================================================================== */
/*  Member 4: SD card on its OWN SPI bus (host SPI2_HOST / HSPI)        */
/*                                                                      */
/*  The old plan used 13/14/26/27 for the SD card, but those are        */
/*  already taken by the PIR sensors, room 2 LED and entry servo.       */
/*  These four pins are free and avoid every pin above.                 */
/*  GPIO 35 is input-only, which is fine because MISO is an input.      */
/* ================================================================== */
#define PIN_SD_MOSI         17   /* [new] */
#define PIN_SD_SCK          16   /* [new] */
#define PIN_SD_MISO         35   /* [new] input-only pin is OK here     */
#define PIN_SD_CS           32   /* [new] */

/* Intrusion alarm outputs (Member 4's after-hours alert) */
#define PIN_ALERT_LED        2   /* [new] red LED; free now SD CS left 2 */
#define PIN_BUZZER          12   /* [new] see strapping warning below   */

/* ================================================================== */
/*  Member 5: I2C LCD (PCF8574 backpack) and serial console             */
/* ================================================================== */
#define PIN_I2C_SDA         21   /* [new] LCD data                      */
#define PIN_I2C_SCL         22   /* [new] LCD clock                     */

#define PIN_UART_TX          1   /* [as-is] USB serial console (UART0)  */
#define PIN_UART_RX          3   /* [as-is] USB serial console (UART0)  */

/*
 * WARNING (real hardware only; Wokwi does not care):
 *   GPIO 2, 12 and 15 are "strapping" pins read by the chip at boot.
 *   GPIO 12 must be LOW at boot, so a buzzer on it can stop a real board
 *   from starting. If you move to real hardware, put the buzzer elsewhere.
 *
 * FREE PINS LEFT: none that are output-capable except 0 (boot button).
 *   36 and 39 are input-only. So the board is now full.
 */

#endif /* PINS_H */


