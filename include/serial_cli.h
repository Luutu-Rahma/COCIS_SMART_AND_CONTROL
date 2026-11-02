/*
 * serial_cli.h  --  MEMBER 5: Serial Interface & System Diagnostics
 *
 * Public interface. Other members only need serial_cli_init() and the task.
 */
#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include "esp_err.h"

/* Sets up the UART driver and the optional I2C LCD. Call ONCE from app_main,
 * AFTER access_control_init() (the CLI reads the headcount). */
esp_err_t serial_cli_init(void);

/* FreeRTOS task body: reads typed commands, samples statistics, refreshes
 * the LCD. Start it with:
 *   xTaskCreate(vTaskSerialCLI, "cli", 4096, NULL, 3, NULL);            */
void vTaskSerialCLI(void *pvParameters);

/* Runs one command line, e.g. "status" or "login cocis2026".
 * The line is modified in place (it is split into words). */
void parse_cli_command(char *line);

#endif /* SERIAL_CLI_H */