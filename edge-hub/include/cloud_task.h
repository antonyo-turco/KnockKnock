#ifndef CLOUD_TASK_H
#define CLOUD_TASK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void cloud_task_init(void);
esp_err_t cloud_publish_alarm(const uint8_t *mac, uint8_t alarm_code);
esp_err_t cloud_publish_response(const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_TASK_H */
