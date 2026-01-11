#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// WiFi status callback type
typedef void (*wifi_status_callback_t)(bool connected, void* user_data);

/**
 * @brief Initialize WiFi in station mode and connect to AP
 * 
 * @param ssid WiFi SSID to connect to
 * @param password WiFi password
 * @return true if connected successfully, false otherwise
 */
bool wifi_init_sta(const char* ssid, const char* password);

/**
 * @brief Set callback for WiFi status changes
 * 
 * @param callback Function to call when WiFi connects/disconnects
 * @param user_data Pointer passed to callback
 */
void wifi_set_status_callback(wifi_status_callback_t callback, void* user_data);

/**
 * @brief Check if WiFi is currently connected
 * 
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
