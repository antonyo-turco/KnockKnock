#include "led_indicator.h"
#include "config.h"

#ifdef CONFIG_IDF_TARGET_ESP32C3

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_timer_handle_t s_train_blink_timer = NULL;
static TaskHandle_t       s_sos_task_handle   = NULL;

static inline void led_on(void)  { gpio_set_level(MY_PIN_LED, 0); }
static inline void led_off(void) { gpio_set_level(MY_PIN_LED, 1); }

static void train_blink_cb(void *arg) {
    static bool s = true;
    s = !s;
    gpio_set_level(MY_PIN_LED, s ? 0 : 1);
}

void led_training_start(void) {
    if (s_train_blink_timer) return;
    const esp_timer_create_args_t args = { .callback = train_blink_cb,
                                           .name = "led_train" };
    esp_timer_create(&args, &s_train_blink_timer);
    led_on();
    esp_timer_start_periodic(s_train_blink_timer, 1000000ULL);
}

void led_training_stop(void) {
    if (!s_train_blink_timer) return;
    esp_timer_stop(s_train_blink_timer);
    esp_timer_delete(s_train_blink_timer);
    s_train_blink_timer = NULL;
    led_off();
}

/* SOS timing: · = 200 ms on / 150 ms off, — = 600 ms on / 150 ms off,
 * 300 ms between letters, 1500 ms pause between repetitions. */
#define SOS_DOT_ON_MS    200
#define SOS_DOT_OFF_MS   150
#define SOS_DASH_ON_MS   600
#define SOS_DASH_OFF_MS  150
#define SOS_LETTER_MS    300
#define SOS_PAUSE_MS    1500

static void sos_task(void *arg) {
    while (1) {
        for (int i = 0; i < 3; i++) {
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DOT_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DOT_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_LETTER_MS));
        for (int i = 0; i < 3; i++) {
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DASH_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DASH_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_LETTER_MS));
        for (int i = 0; i < 3; i++) {
            led_on();  vTaskDelay(pdMS_TO_TICKS(SOS_DOT_ON_MS));
            led_off(); vTaskDelay(pdMS_TO_TICKS(SOS_DOT_OFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(SOS_PAUSE_MS));
    }
}

void led_sos_start(void) {
    if (s_sos_task_handle) return;
    xTaskCreate(sos_task, "led_sos", 1024, NULL, 3, &s_sos_task_handle);
}

void led_sos_stop(void) {
    if (!s_sos_task_handle) return;
    vTaskDelete(s_sos_task_handle);
    s_sos_task_handle = NULL;
    led_off();
}

#else

void led_sos_start(void)      {}
void led_sos_stop(void)       {}
void led_training_start(void) {}
void led_training_stop(void)  {}

#endif /* CONFIG_IDF_TARGET_ESP32C3 */
