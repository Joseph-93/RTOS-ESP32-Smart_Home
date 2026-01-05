#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in station mode and connect to AP
 * 
 * @param ssid WiFi SSID to connect to
 * @param password WiFi password
 * @return true if connected successfully, false otherwise
 */
bool wifi_init_sta(const char* ssid, const char* password);

#ifdef __cplusplus
}
#endif
