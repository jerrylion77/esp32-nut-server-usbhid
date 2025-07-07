idf_component_register(SRCS "esp32-nut-server-usbhid.c" "webserver.c"
                    INCLUDE_DIRS "."
                    REQUIRES usb esp_wifi esp_http_server nvs_flash json esp_timer)
