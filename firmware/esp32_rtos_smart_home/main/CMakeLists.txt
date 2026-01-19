idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES common gui network_actions light_sensor motion_sensor door_sensor web_server)
