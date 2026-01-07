#include "tcp_client.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <unistd.h>
#include <sstream>

static const char *TAG = "TcpClient";

TcpClient::TcpClient() : initialized(false) {}

TcpClient::~TcpClient() {
    cleanup();
}

bool TcpClient::initialize() {
    ESP_LOGI(TAG, "Initializing TCP client");
    initialized = true;
    return true;
}

void TcpClient::cleanup() {
    ESP_LOGI(TAG, "Cleaning up TCP client - closing all sockets");
    for (auto& pair : socket_pool) {
        if (pair.second >= 0) {
            close(pair.second);
            ESP_LOGI(TAG, "Closed socket %s (FD=%d)", pair.first.c_str(), pair.second);
        }
    }
    socket_pool.clear();
}

int TcpClient::getOrCreateSocket(const std::string& host, uint16_t port, uint32_t timeout_ms) {
    std::stringstream key_stream;
    key_stream << host << ":" << port;
    std::string key = key_stream.str();
    
    // Check if we already have a connection
    auto it = socket_pool.find(key);
    if (it != socket_pool.end() && it->second >= 0) {
        ESP_LOGI(TAG, "Reusing existing socket for %s (FD=%d)", key.c_str(), it->second);
        return it->second;
    }
    
    ESP_LOGI(TAG, "Creating new socket for %s", key.c_str());
    
    // Create new socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return -1;
    }
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Resolve hostname
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host.c_str());
        close(sock);
        return -1;
    }
    
    // Setup address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    memcpy(&dest_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    
    // Connect
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d errno %d", host.c_str(), port, errno);
        close(sock);
        return -1;
    }
    
    ESP_LOGI(TAG, "Connected to %s (FD=%d)", key.c_str(), sock);
    socket_pool[key] = sock;
    return sock;
}

void TcpClient::closeSocket(const std::string& key) {
    auto it = socket_pool.find(key);
    if (it != socket_pool.end() && it->second >= 0) {
        close(it->second);
        ESP_LOGI(TAG, "Closed socket %s (FD=%d)", key.c_str(), it->second);
        socket_pool.erase(it);
    }
}

bool TcpClient::send(const TcpMessage& msg) {
    if (!initialized) {
        ESP_LOGE(TAG, "TCP client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending TCP message '%s' to %s:%d", 
             msg.name.c_str(), msg.host.c_str(), msg.port);
    
    int sock = getOrCreateSocket(msg.host, msg.port, msg.timeout_ms);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to get/create socket");
        return false;
    }
    
    // Send data
    int sent = lwip_send(sock, msg.data.c_str(), msg.data.length(), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed: errno %d - closing socket", errno);
        std::stringstream key_stream;
        key_stream << msg.host << ":" << msg.port;
        closeSocket(key_stream.str());
        return false;
    }
    
    ESP_LOGI(TAG, "Sent %d bytes to %s:%d", sent, msg.host.c_str(), msg.port);
    
    // Try to receive response
    char rx_buffer[512];
    int rx_len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (rx_len > 0) {
        rx_buffer[rx_len] = '\0';
        ESP_LOGI(TAG, "TCP Response (%d bytes): %s", rx_len, rx_buffer);
    } else if (rx_len == 0) {
        ESP_LOGI(TAG, "TCP Response: Connection closed by peer");
        std::stringstream key_stream;
        key_stream << msg.host << ":" << msg.port;
        closeSocket(key_stream.str());
    } else {
        ESP_LOGW(TAG, "TCP Response: No data received (errno %d)", errno);
    }
    
    return true;
}
