#pragma once

#include <string>
#include <cstdint>
#include <map>

// Compact struct for a TCP message configuration
struct TcpMessage {
    std::string name;
    std::string host;
    uint16_t port;
    std::string data;
    uint32_t timeout_ms;
    
    TcpMessage(const std::string& name, const std::string& host, uint16_t port, 
               const std::string& data, uint32_t timeout_ms = 5000)
        : name(name), host(host), port(port), data(data), timeout_ms(timeout_ms) {}
};

// TCP client for fire-and-forget sends
class TcpClient {
public:
    TcpClient();
    ~TcpClient();
    
    bool initialize();
    bool send(const TcpMessage& msg);
    void cleanup();
    
private:
    bool initialized;
    // Socket pool: key is "host:port", value is socket FD
    std::map<std::string, int> socket_pool;
    
    int getOrCreateSocket(const std::string& host, uint16_t port, uint32_t timeout_ms);
    void closeSocket(const std::string& key);
};
