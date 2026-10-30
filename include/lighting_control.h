#ifndef LIGHTING_CONTROL_H
#define LIGHTING_CONTROL_H

#include "driver/gpio.h"
#include "driver/ledc.h"

#define PIR_ROOM1_PIN  GPIO_NUM_13
#define PIR_ROOM2_PIN  GPIO_NUM_14
#define LED_ROOM1_PIN  GPIO_NUM_25
#define LED_ROOM2_PIN  GPIO_NUM_26
#define POTENTIOMETER_PIN GPIO_NUM_34

typedef enum{
    ROOM_VACANT,
    ROOM_STANDBY,
    ROOM_OCCUPIED
} RoomState_t;

void lighting_control_init(void);
void vTaskLightingControl(void *pvParameters);
RoomState_t lighting_get_room_state(int room);
bool lighting_get_motion(int room);
uint8_t lighting_get_brightness_pct(int room);
void lighting_set_evacuation(bool active);

#endif
