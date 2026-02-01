// Subscription management methods implementation
// NOTE: This file is now deprecated - subscription logic is in web_server.cpp
// This file exists only for backward compatibility during the transition.
// The new API uses param_id (UUID) instead of component/type/index.

#include "web_server.h"
#include "esp_log.h"

// unsubscribe_param - remove a subscription for a specific parameter
void WebServerComponent::unsubscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        auto it = subscriptions.find(socket_fd);
        if (it != subscriptions.end()) {
            it->second.erase(key);
            if (it->second.empty()) {
                subscriptions.erase(it);
            }
        }
        xSemaphoreGive(subscriptions_mutex);
    }
}

// clear_subscriptions - remove all subscriptions for a socket (on disconnect)
void WebServerComponent::clear_subscriptions(int socket_fd) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        auto it = subscriptions.find(socket_fd);
        if (it != subscriptions.end()) {
            int count = it->second.size();
            subscriptions.erase(it);
            ESP_LOGI(TAG, "Cleared %d subscriptions for socket %d", count, socket_fd);
        }
        xSemaphoreGive(subscriptions_mutex);
    }
}
