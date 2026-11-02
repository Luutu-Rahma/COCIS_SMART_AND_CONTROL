#include "lighting_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "LIGHTING_CONTROL";

// PWM
#define PWM_DUTY_FULL 8191
#define PWM_DUTY_STANDBY 2457
#define PWM_DUTY_OFF 0

#define GRACE_PERIOD_MS 30000 // 30 second grace timer

// Timer 0 and channels 0-1 are reserved for the access-control servos.
#define LIGHT_LEDC_TIMER LEDC_TIMER_1
#define LIGHT_LEDC_CH_ROOM1 LEDC_CHANNEL_2
#define LIGHT_LEDC_CH_ROOM2 LEDC_CHANNEL_3

static RoomState_t s_room_state[2] = {ROOM_VACANT, ROOM_VACANT};
static bool s_room_motion[2] = {false, false};
static TickType_t s_last_motion[2] = {0, 0};
static SemaphoreHandle_t s_light_mutex = NULL;
static bool s_evacuation = false;

static int room_index(int room)
{
    return room == 1 ? 0 : (room == 2 ? 1 : -1);
}

static void set_room_duty(int index, uint32_t duty)
{
    ledc_channel_t channel = index == 0 ? LIGHT_LEDC_CH_ROOM1 : LIGHT_LEDC_CH_ROOM2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

void lighting_control_init(void)
{
    s_light_mutex = xSemaphoreCreateMutex();
    if (s_light_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create lighting state mutex");
        return;
    }

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
        .timer_num = LIGHT_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&ledc_timer);

    // configurig LEDC Channel 2 for room 1
    ledc_channel_config_t room1_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LIGHT_LEDC_CH_ROOM1,
        .timer_sel = LIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_ROOM1_PIN,
        .duty = PWM_DUTY_OFF,
        .hpoint = 0};
    ledc_channel_config(&room1_channel);

    // configuring LEDC channel 3 for room 2
    ledc_channel_config_t room2_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LIGHT_LEDC_CH_ROOM2,
        .timer_sel = LIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_ROOM2_PIN,
        .duty = PWM_DUTY_OFF,
        .hpoint = 0};
    ledc_channel_config(&room2_channel);

    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_light_mutex, portMAX_DELAY);
    s_last_motion[0] = now;
    s_last_motion[1] = now;
    xSemaphoreGive(s_light_mutex);
    ESP_LOGI(TAG, "LIGHTING CONTROL INITIALIZED SUCCESSFULLY..");
}

RoomState_t lighting_get_room_state(int room)
{
    int index = room_index(room);
    if (index < 0 || s_light_mutex == NULL) return ROOM_VACANT;
    xSemaphoreTake(s_light_mutex, portMAX_DELAY);
    RoomState_t state = s_room_state[index];
    xSemaphoreGive(s_light_mutex);
    return state;
}

bool lighting_get_motion(int room)
{
    int index = room_index(room);
    if (index < 0 || s_light_mutex == NULL) return false;
    xSemaphoreTake(s_light_mutex, portMAX_DELAY);
    bool motion = s_room_motion[index];
    xSemaphoreGive(s_light_mutex);
    return motion;
}

uint8_t lighting_get_brightness_pct(int room)
{
    int index = room_index(room);
    if (index < 0 || s_light_mutex == NULL) return 0;
    xSemaphoreTake(s_light_mutex, portMAX_DELAY);
    uint8_t brightness = s_evacuation ? 100 :
        (s_room_state[index] == ROOM_OCCUPIED ? 100 :
         (s_room_state[index] == ROOM_STANDBY ? 30 : 0));
    xSemaphoreGive(s_light_mutex);
    return brightness;
}

void lighting_set_evacuation(bool active)
{
    if (s_light_mutex == NULL) return;
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_light_mutex, portMAX_DELAY);
    bool changed = s_evacuation != active;
    s_evacuation = active;
    if (!active && changed) {
        s_room_state[0] = ROOM_VACANT;
        s_room_state[1] = ROOM_VACANT;
        s_last_motion[0] = now;
        s_last_motion[1] = now;
    }
    xSemaphoreGive(s_light_mutex);

    if (changed && !active) {
        set_room_duty(0, PWM_DUTY_OFF);
        set_room_duty(1, PWM_DUTY_OFF);
    }
}

void vTaskLightingControl(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        bool motion[2] = {
            gpio_get_level(PIR_ROOM1_PIN) == 1,
            gpio_get_level(PIR_ROOM2_PIN) == 1};
        TickType_t now = xTaskGetTickCount();
        bool evacuating;

        xSemaphoreTake(s_light_mutex, portMAX_DELAY);
        s_room_motion[0] = motion[0];
        s_room_motion[1] = motion[1];
        evacuating = s_evacuation;
        if (!evacuating) {
            for (int i = 0; i < 2; i++) {
                if (motion[i]) s_last_motion[i] = now;
            }
        }
        xSemaphoreGive(s_light_mutex);

        if (evacuating) {
            set_room_duty(0, PWM_DUTY_FULL);
            set_room_duty(1, PWM_DUTY_FULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (int i = 0; i < 2; i++) {
            RoomState_t old_state;
            RoomState_t new_state;
            uint32_t elapsed_ms;
            xSemaphoreTake(s_light_mutex, portMAX_DELAY);
            old_state = s_room_state[i];
            elapsed_ms = (now - s_last_motion[i]) * portTICK_PERIOD_MS;
            new_state = old_state;
            if (motion[i]) {
                new_state = ROOM_OCCUPIED;
            } else if (elapsed_ms > GRACE_PERIOD_MS && old_state == ROOM_STANDBY) {
                new_state = ROOM_VACANT;
            } else if (elapsed_ms <= GRACE_PERIOD_MS && old_state == ROOM_OCCUPIED) {
                new_state = ROOM_STANDBY;
            }
            if (new_state != old_state) s_room_state[i] = new_state;
            xSemaphoreGive(s_light_mutex);

            if (new_state != old_state) {
                uint32_t duty = new_state == ROOM_OCCUPIED ? PWM_DUTY_FULL :
                    (new_state == ROOM_STANDBY ? PWM_DUTY_STANDBY : PWM_DUTY_OFF);
                set_room_duty(i, duty);
                if (i == 0) {
                    if (new_state == ROOM_OCCUPIED) ESP_LOGI(TAG, "ROOM 1: MOTION DETECTED....100%% BRIGHTNESS");
                    else if (new_state == ROOM_STANDBY) ESP_LOGI(TAG, "ROOM 1: MOTION STOPPED... 30%% STAND BY");
                    else ESP_LOGI(TAG, "ROOM 1: GRACE PERIOD EXPIRED....LIGHTS OFF");
                } else {
                    if (new_state == ROOM_OCCUPIED) ESP_LOGI(TAG, "ROOM 2: MOTION DETECTED....100%% BRIGHTNESS");
                    else if (new_state == ROOM_STANDBY) ESP_LOGI(TAG, "ROOM 2: MOTION STOPPED... 30%% STAND BY");
                    else ESP_LOGI(TAG, "ROOM 2: GRACE PERIOD EXPIRED....LIGHTS OFF");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
