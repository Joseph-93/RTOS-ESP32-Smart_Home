idf_component_register(
    SRCS "ota_update.cpp"
    INCLUDE_DIRS "."
    REQUIRES common app_update esp_http_client
)
