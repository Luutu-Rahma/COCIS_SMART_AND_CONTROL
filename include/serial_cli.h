#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include "esp_err.h"

/* initialize UART and optional LCD after access_control_init */
esp_err_t serial_cli_init(void);

/* FreeRTOS task body */
void vTaskSerialCLI(void *pvParameters);

/* execute a command line; line is modified in place */
void parse_cli_command(char *line);

#endif /* SERIAL_CLI_H */
