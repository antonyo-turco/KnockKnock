idf_component_register(SRCS "esp_now_comm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi secure_store esp_event)
