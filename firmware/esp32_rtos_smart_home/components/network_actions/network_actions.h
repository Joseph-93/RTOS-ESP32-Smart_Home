#pragma once

#include "../component.h"
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
    
    void initialize() override;
    
    // Send methods - return true on success, false on failure
    // By index
    bool sendTcp(size_t index);
    bool sendHttp(size_t index);
    bool sendWs(size_t index);
    
    // Index lookup helpers (one-time lookup for efficient repeated sends)
    size_t getTcpMessageIdx(const std::string& name) const;
    size_t getHttpMessageIdx(const std::string& name) const;
    size_t getWsMessageIdx(const std::string& name) const;
    
    // Pointer getters by name
    const TcpMessage* getTcpMessage(const std::string& name) const;
    const HttpMessage* getHttpMessage(const std::string& name) const;
    const WsMessage* getWsMessage(const std::string& name) const;
    
    // Getters for message counts (useful for GUI)
    size_t getTcpMessageCount() const { return tcp_messages.size(); }
    size_t getHttpMessageCount() const { return http_messages.size(); }
    size_t getWsMessageCount() const { return ws_messages.size(); }
    
    const std::vector<TcpMessage>& getTcpMessages() const { return tcp_messages; }
    const std::vector<HttpMessage>& getHttpMessages() const { return http_messages; }
    const std::vector<WsMessage>& getWsMessages() const { return ws_messages; }
    
private:
    // Client instances
    TcpClient tcp_client;
    HttpClient http_client;
    WsClient ws_client;
    
    // Parsed message configurations
    std::vector<TcpMessage> tcp_messages;
    std::vector<HttpMessage> http_messages;
    std::vector<WsMessage> ws_messages;
    
    // Message loading and parsing helpers
    void loadAllMessageExamples();
    void parseTcpMessages();
    void parseHttpMessages();
    void parseWsMessages();
    
    // Action registration
    void registerActions();
};
#endif
