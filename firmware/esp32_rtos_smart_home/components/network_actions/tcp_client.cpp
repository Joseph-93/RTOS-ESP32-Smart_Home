#include "tcp_client.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <unistd.h>

static const char *TAG = "TcpClient";

TcpClient::TcpClient() : initialized(false) {}

TcpClient::~TcpClient() {}

bool TcpClient::initialize() {
    ESP_LOGI(TAG, "Initializing TCP client");
    initialized = true;
    return true;
}

bool TcpClient::send(const TcpMessage& msg) {
    if (!initialized) {
        ESP_LOGE(TAG, "TCP client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending TCP message '%s' to %s:%d", 
             msg.name.c_str(), msg.host.c_str(), msg.port);
    
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return false;
    }
    
    ESP_LOGI(TAG, "TCP socket created: FD=%d", sock);
    bool success = false;
    int sent = 0;  // Declare before any goto
    int rx_len = 0;  // Declare before any goto
    char rx_buffer[512];
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = msg.timeout_ms / 1000;
    timeout.tv_usec = (msg.timeout_ms % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set RX timeout");
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set TX timeout");
    }
    
    // Resolve hostname
    struct hostent *server = gethostbyname(msg.host.c_str());
    if (server == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", msg.host.c_str());
        goto cleanup;
    }
    
    // Setup address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(msg.port);
    memcpy(&dest_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    
    // Connect
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d errno %d", msg.host.c_str(), msg.port, errno);
        goto cleanup;
    }
    
    // Send data
    sent = lwip_send(sock, msg.data.c_str(), msg.data.length(), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "Sent %d bytes to %s:%d", sent, msg.host.c_str(), msg.port);
    
    // Try to receive response
    rx_len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (rx_len > 0) {
        rx_buffer[rx_len] = '\0';
        ESP_LOGI(TAG, "TCP Response (%d bytes): %s", rx_len, rx_buffer);
    } else if (rx_len == 0) {
        ESP_LOGI(TAG, "TCP Response: Connection closed by peer");
    } else {
        ESP_LOGW(TAG, "TCP Response: No data received (errno %d)", errno);
    }
    
    success = true;
    
cleanup:
    ESP_LOGI(TAG, "CLOSING TCP socket FD=%d", sock);
    int close_result = close(sock);
    if (close_result != 0) {
        ESP_LOGE(TAG, "!!!! CRITICAL: FAILED TO CLOSE SOCKET FD=%d errno=%d !!!!", sock, errno);
        ESP_LOGE(TAG, "!!!! FILE DESCRIPTOR LEAK DETECTED - SYSTEM WILL FAIL !!!!");
        assert(false && "TCP SOCKET CLOSE FAILED - FD LEAK");
    }
    ESP_LOGI(TAG, "TCP socket FD=%d closed successfully", sock);
    return success;
}
