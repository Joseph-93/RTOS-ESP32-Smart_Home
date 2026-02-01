idf_component_register(SRCS "component_graph.cpp" "component.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES freertos esp_common esp_timer json
                       PRIV_REQUIRES nvs_flash)
