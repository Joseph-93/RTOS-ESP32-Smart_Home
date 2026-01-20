// Subscription management methods implementation
#include "web_server.h"
#include "esp_log.h"

void WebServerComponent::subscribe_param(int socket_fd, const SubscriptionKey& key) {
    subscriptions[socket_fd].insert(key);
    ESP_LOGI(TAG, "Socket %d subscribed to %s.%s[%d][%d][%d]. Total subscriptions for this socket: %d",
             socket_fd, key.component.c_str(), key.param_type.c_str(), 
             key.idx, key.row, key.col, subscriptions[socket_fd].size());
}

void WebServerComponent::unsubscribe_param(int socket_fd, const SubscriptionKey& key) {
    auto it = subscriptions.find(socket_fd);
    if (it != subscriptions.end()) {
        it->second.erase(key);
        if (it->second.empty()) {
            subscriptions.erase(it);
        }
    }
}

void WebServerComponent::clear_subscriptions(int socket_fd) {
    auto it = subscriptions.find(socket_fd);
    if (it != subscriptions.end()) {
        int count = it->second.size();
        subscriptions.erase(it);
        ESP_LOGI(TAG, "Cleared %d subscriptions for socket %d", count, socket_fd);
    }
}

void WebServerComponent::broadcastParameterUpdate(const char* comp, const char* param_type, 
                                                   int idx, int row, int col, cJSON* value) {
    if (!http_server) return;
    
    SubscriptionKey key{comp, param_type, idx, row, col};
    
    // Find all sockets subscribed to this parameter
    for (const auto& [socket_fd, subscribed_params] : subscriptions) {
        if (subscribed_params.find(key) != subscribed_params.end()) {
            // Build push message
            cJSON* push_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(push_msg, "type", "param_update");
            cJSON_AddStringToObject(push_msg, "comp", comp);
            cJSON_AddStringToObject(push_msg, "param_type", param_type);
            cJSON_AddNumberToObject(push_msg, "idx", idx);
            cJSON_AddNumberToObject(push_msg, "row", row);
            cJSON_AddNumberToObject(push_msg, "col", col);
            
            // Add value (duplicate the cJSON node)
            cJSON_AddItemToObject(push_msg, "value", cJSON_Duplicate(value, 1));
            
            char* msg_str = cJSON_PrintUnformatted(push_msg);
            if (msg_str) {
                // Send to this socket
                httpd_ws_frame_t ws_pkt;
                memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                ws_pkt.payload = (uint8_t*)msg_str;
                ws_pkt.len = strlen(msg_str);
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                
                esp_err_t ret = httpd_ws_send_frame_async(http_server, socket_fd, &ws_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send param update to socket %d: %d. Clearing subscriptions.", 
                             socket_fd, ret);
                    clear_subscriptions(socket_fd);
                }
                
                free(msg_str);
            }
            
            cJSON_Delete(push_msg);
        }
    }
}
