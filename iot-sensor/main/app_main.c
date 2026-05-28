// Wrapper to forward ESP-IDF app_main to src/main.c
extern void app_main(void);

void app_main(void) __attribute__((weak));
void app_main(void) {
    // This should be overridden by the app_main in src/main.c
}
