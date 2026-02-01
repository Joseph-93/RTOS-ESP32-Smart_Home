idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES common gui heartbeat light_sensor motion_sensor door_sensor web_server wifi)
