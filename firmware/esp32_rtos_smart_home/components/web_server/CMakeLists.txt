idf_component_register(
    SRCS "web_server_subscription.cpp" "web_server.cpp"
    INCLUDE_DIRS "."
    REQUIRES common json esp_http_server
)
