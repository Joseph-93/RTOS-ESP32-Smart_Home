idf_component_register(SRCS "component.cpp" "component_graph.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES freertos esp_common esp_timer json)
