#include "lighting_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "LIGHTING_CONTROL";

// PWM
#define PWM_DUTY_FULL 8191
#define PWM_DUTY_STANDBY 2457
#define PWM_DUTY_OFF 0

#define GRACE_PERIOD_MS 30000 // 30 second grace timer

void lighting_control_init(void)
{
    // configuring snesors
    gpio_config_t pir_config = {
        .pin_bit_mask = (1ULL << PIR_ROOM1_PIN) | (1ULL << PIR_ROOM2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&pir_config);

    // configuring PWM ledc timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&ledc_timer);

    // configurig LEDC Channel 0 for room 1
    ledc_channel_config_t room1_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_ROOM1_PIN,
        .duty = PWM_DUTY_OFF,
        .hpoint = 0};
    ledc_channel_config(&room1_channel);

    // configuring LEDC channel 1 for room 2
    ledc_channel_config_t room2_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_ROOM2_PIN,
        .duty = PWM_DUTY_OFF,
        .hpoint = 0};
    ledc_channel_config(&room2_channel);

    ESP_LOGI(TAG, "LIGHTING CONTROL INITIALIZED SUCCESSFULLY..");
}

void vTaskLightingControl(void *pvParameters)
{
    TickType_t last_motion_room1 = xTaskGetTickCount();
    TickType_t last_motion_room2 = xTaskGetTickCount();

    RoomState_t state_room1 = ROOM_VACANT;
    RoomState_t state_room2 = ROOM_VACANT;

    while (1)
    {
        int pir1 = gpio_get_level(PIR_ROOM1_PIN);
        int pir2 = gpio_get_level(PIR_ROOM2_PIN);

        // LECTURE ROOM 1
        if (pir1 == 1)
        {
            last_motion_room1 = xTaskGetTickCount();
            if (state_room1 != ROOM_OCCUPIED)
            {
                state_room1 = ROOM_OCCUPIED;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, PWM_DUTY_FULL);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                ESP_LOGI(TAG, "ROOM 1: MOTION DETECTED....100%% BRIGHTNESS");
            }
        }
        else
        {
            uint32_t elapsed_ms = (xTaskGetTickCount() - last_motion_room1) * portTICK_PERIOD_MS;
            if (elapsed_ms > GRACE_PERIOD_MS && state_room1 == ROOM_STANDBY)
            {
                state_room1 = ROOM_VACANT;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, PWM_DUTY_OFF);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                ESP_LOGI(TAG, "ROOM 1: GRACE PERIOD EXPIRED....LIGHTS OFF");
            }
            else if (
                elapsed_ms <= GRACE_PERIOD_MS && state_room1 == ROOM_OCCUPIED)
            {
                state_room1 = ROOM_STANDBY;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, PWM_DUTY_STANDBY);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
                ESP_LOGI(TAG, "ROOM 1: MOTION STOPPED... 30%% STAND BY");
            }
        }

        //ROOM 2 
        if (pir2 == 1){
            last_motion_room2 = xTaskGetTickCount();
            if (state_room2 != ROOM_OCCUPIED){
                state_room2 = ROOM_OCCUPIED;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, PWM_DUTY_FULL);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                ESP_LOGI(TAG, "ROOM 2: MOTION DETECTED....100%% BRIGHTNESS");
            } 
        }
        else {
            uint32_t elapsed_ms = (xTaskGetTickCount() -last_motion_room2) *portTICK_PERIOD_MS;
            if (elapsed_ms > GRACE_PERIOD_MS && state_room2 == ROOM_STANDBY){
                state_room2 = ROOM_VACANT;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, PWM_DUTY_OFF);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                ESP_LOGI(TAG, "ROOM 2: GRACE PERIOD EXPIRED....LIGHTS OFF");
            }
            else if 
            (elapsed_ms <= GRACE_PERIOD_MS && state_room2 == ROOM_OCCUPIED){
                state_room2 = ROOM_STANDBY;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, PWM_DUTY_STANDBY);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                ESP_LOGI(TAG, "ROOM 2: MOTION STOPPED... 30%% STAND BY");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
