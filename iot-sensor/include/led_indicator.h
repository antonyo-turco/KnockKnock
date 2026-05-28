#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

/* LED status patterns for the ESP32-C3 Super Mini (GPIO 8, active-LOW).
 * All functions are no-ops on targets without MY_PIN_LED defined. */

void led_sos_start(void);
void led_sos_stop(void);
void led_training_start(void);
void led_training_stop(void);

#endif // LED_INDICATOR_H
