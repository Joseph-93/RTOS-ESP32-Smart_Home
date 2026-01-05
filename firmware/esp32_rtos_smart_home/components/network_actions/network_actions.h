#pragma once

#include "../component.h"

#ifdef __cplusplus

// NetworkActionsComponent - manages network communication parameters
class NetworkActionsComponent : public Component {
public:
    NetworkActionsComponent();
    ~NetworkActionsComponent() override;
    
    void initialize() override;
};

extern "C" {
#endif

/**
 * @brief Initialize network actions system
 * 
 * Sets up the network action manager for smart home messages
 */
void network_actions_init(void);

/**
 * @brief Send a simple action message
 * 
 * @param action Action name (e.g., "light_on", "temp_set")
 * @param value Action value or parameter
 */
void network_actions_send(const char *action, const char *value);

#ifdef __cplusplus
}
#endif
