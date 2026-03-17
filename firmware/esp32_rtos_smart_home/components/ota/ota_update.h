#pragma once

#include "component.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * OTA Update Component
 * 
 * Enables wireless firmware updates over WiFi. Always included in every build
 * so that any ESP32 can be updated without physical access.
 * 
 * Read-only parameters (subscribable for live progress tracking):
 *   - status:    "idle", "connecting", "downloading", "verifying", "rebooting", "error: ..."
 *   - progress:  0-100 download percentage
 *   - partition: label of the currently running partition (e.g. "ota_0", "ota_1")
 * 
 * Triggered via WebSocket message:
 *   {"type": "start_ota", "url": "http://192.168.1.x:port/firmware.bin"}
 * 
 * After a successful OTA update, the ESP32 reboots into the new firmware.
 * On first boot after OTA, the firmware is automatically validated (rollback cancelled).
 * If the new firmware crashes before validation, the bootloader rolls back to the previous version.
 */
class OtaUpdateComponent : public Component {
public:
    OtaUpdateComponent();
    
    void onInitialize() override;
    
    /**
     * Start an OTA update by downloading firmware from the given URL.
     * Returns true if the OTA task was successfully started.
     * The actual download and flash happens asynchronously in a background task.
     */
    bool startUpdate(const char* url);
    
    /** Check if an OTA update is currently in progress. */
    bool isUpdateInProgress() const { return update_in_progress; }
    
    static constexpr const char* TAG = "OTA";
    
private:
    StringParameter* status;         // Current OTA state (read-only)
    IntParameter* progress;          // Download progress 0-100 (read-only)
    StringParameter* partition_info; // Current running partition label (read-only)
    
    TaskHandle_t ota_task_handle = nullptr;
    volatile bool update_in_progress = false;
    char update_url[256];
    
    static void otaTaskWrapper(void* pvParameters);
    void otaTask();
    
    /** On first boot after OTA, validate the firmware and cancel rollback. */
    void markAppValid();
};

// ============================================================================
// C API for cross-component access (avoids header circular dependencies)
// These are called from ComponentGraph::executeMessage() via extern "C"
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/** Start OTA update from URL. Returns true if started successfully. */
bool ota_start_update(const char* url);

/** Check if OTA update is in progress. */
bool ota_is_update_in_progress(void);

#ifdef __cplusplus
}
#endif
