idf_component_register(SRCS "network_actions.cpp" "tcp_client.cpp" "http_client.cpp" "ws_client.cpp" "wifi_init.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_event esp_http_client json lwip esp_wifi nvs_flash espressif__esp_websocket_client)
