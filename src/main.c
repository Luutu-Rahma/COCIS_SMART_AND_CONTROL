#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lighting_control.h"

void app_main(void){
    lighting_control_init();

    xTaskCreate(
        vTaskLightingControl,
        "Lighting task",
        2048,
        NULL,
        2,
        NULL
    );
}