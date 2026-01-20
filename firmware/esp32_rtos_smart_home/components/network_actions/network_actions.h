#pragma once

#include "component.h"
#include "tcp_client.h"
#include "http_client.h"
#include "ws_client.h"
#include <vector>

#ifdef __cplusplus

// NetworkActionsComponent - manages network communication parameters
class NetworkActionsComponent : public Component {
public:
    NetworkActionsComponent();
    ~NetworkActionsComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void onInitialize() override;
    
    // Send methods - return true on success, false on failure
    // By index
    bool sendTcp(size_t index);
    bool sendHttp(size_t index);
    bool sendWs(size_t index);
    
    // Index lookup helpers (one-time lookup for efficient repeated sends)
    size_t getTcpMessageIdx(const std::string& name) const;
    size_t getHttpMessageIdx(const std::string& name) const;
    size_t getWsMessageIdx(const std::string& name) const;
    
    // Parse message by name into output struct (returns false if not found)
    bool getTcpMessage(const std::string& name, TcpMessage& out) const;
    bool getHttpMessage(const std::string& name, HttpMessage& out) const;
    bool getWsMessage(const std::string& name, WsMessage& out) const;
    
    // Getters for message counts (useful for GUI)
    size_t getTcpMessageCount() const;
    size_t getHttpMessageCount() const;
    size_t getWsMessageCount() const;
    
    static constexpr const char* TAG = "NetworkActions";

private:
    struct NetworkActionQueueItem {
        enum class NetworkProtocol {
            TCP,
            WebSocket,
            HTTP
        } protocol;
        size_t message_index;
    };

    // Client instances
    TcpClient tcp_client;
    HttpClient http_client;
    WsClient ws_client;

    // Message loading helper
    void loadAllMessageExamples();
    
    // On-demand JSON parsing (parses from StringParameter, returns temporary struct)
    bool parseTcpMessageAt(size_t index, TcpMessage& out) const;
    bool parseHttpMessageAt(size_t index, HttpMessage& out) const;
    bool parseWsMessageAt(size_t index, WsMessage& out) const;
    
    // Get message name from JSON at index (for action registration)
    std::string getTcpMessageName(size_t index) const;
    std::string getHttpMessageName(size_t index) const;
    std::string getWsMessageName(size_t index) const;

    // Queue management to make network sends non-blocking for other tasks
    QueueHandle_t network_actions_queue;
    TaskHandle_t network_actions_task_handle;

    static void network_actions_task(void* pvParameters);
    void send_next_from_queue();
    
    // WiFi event callback
    static void wifi_event_callback(bool connected, void* user_data);

    // Action registration
    void registerActions();
};
#endif // __cplusplus

