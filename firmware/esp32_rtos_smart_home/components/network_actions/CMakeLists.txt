idf_component_register(SRCS "network_actions.cpp" "tcp_client.cpp" "http_client.cpp" "ws_client.cpp" "wifi_init.cpp" "../component.cpp"
                       INCLUDE_DIRS "." ".."
                       REQUIRES esp_event esp_http_client json lwip esp_wifi nvs_flash)
