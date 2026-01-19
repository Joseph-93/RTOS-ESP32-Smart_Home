#pragma once

#include "component.h"
#include "component_graph.h"
#include <esp_http_server.h>
#include "cJSON.h"

class WebServerComponent : public Component {
public:
    WebServerComponent();
    ~WebServerComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void initialize() override;

    static constexpr const char* TAG = "WebServer";

private:
    httpd_handle_t http_server = nullptr;
    ComponentGraph* component_graph = nullptr;
    
    // WebSocket handler
    static esp_err_t ws_handler(httpd_req_t *req);
    
    // WebSocket message processor
    cJSON* handle_ws_message(cJSON* request, const char* msg_type);
    
    // Helper methods
    Component* get_component(const char* name);
    static esp_err_t send_json_response(httpd_req_t *req, cJSON* json);
    static esp_err_t send_json_string(httpd_req_t *req, const char* json_str);
};
