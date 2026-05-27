#ifndef HUB_H
#define HUB_H

#ifdef __cplusplus
extern "C" {
#endif

void hub_start(void);
void hub_handle_mqtt_command(const char *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* HUB_H */
