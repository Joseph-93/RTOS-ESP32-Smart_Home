#include "ota_update.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include <string.h>

// Buffer size for downloading firmware chunks
// Keep small - heap is tight when HTTP client + OTA write buffers are active
#define OTA_RECV_BUF_SIZE 1024

// ============================================================================
// C API - static instance for cross-component access
// ============================================================================

static OtaUpdateComponent* s_ota_instance = nullptr;

extern "C" bool ota_start_update(const char* url) {
    if (!s_ota_instance) return false;
    return s_ota_instance->startUpdate(url);
}

extern "C" bool ota_is_update_in_progress(void) {
    if (!s_ota_instance) return false;
    return s_ota_instance->isUpdateInProgress();
}

// ============================================================================
// Constructor
// ============================================================================

OtaUpdateComponent::OtaUpdateComponent() : Component("OTA") {
    s_ota_instance = this;
    
    status = addStringParam("status", 1, 1, "idle", true);
    progress = addIntParam("progress", 1, 1, 0, 100, 0, true);
    partition_info = addStringParam("partition", 1, 1, "unknown", true);
    
    memset(update_url, 0, sizeof(update_url));
}

// ============================================================================
// Initialization
// ============================================================================

void OtaUpdateComponent::onInitialize() {
    // Log which partition we're currently running from
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from partition: %s (addr=0x%lx, size=0x%lx)",
                 running->label, running->address, running->size);
        partition_info->setValue(0, 0, std::string(running->label));
    } else {
        ESP_LOGW(TAG, "Could not determine running partition");
    }
    
    // On first boot after OTA, validate the new firmware
    markAppValid();
    
    // Log the next update partition for debugging
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next) {
        ESP_LOGI(TAG, "Next OTA partition: %s (addr=0x%lx, size=0x%lx)",
                 next->label, next->address, next->size);
    }
}

void OtaUpdateComponent::markAppValid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    
    if (err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "=== First boot after OTA update ===");
        ESP_LOGI(TAG, "Firmware booted successfully - marking as valid (cancelling rollback)");
        esp_ota_mark_app_valid_cancel_rollback();
        status->setValue(0, 0, std::string("validated"));
    }
}

// ============================================================================
// Start Update
// ============================================================================

bool OtaUpdateComponent::startUpdate(const char* url) {
    if (update_in_progress) {
        ESP_LOGW(TAG, "OTA update already in progress - ignoring request");
        return false;
    }
    
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid OTA URL (empty)");
        return false;
    }
    
    if (strlen(url) >= sizeof(update_url)) {
        ESP_LOGE(TAG, "OTA URL too long (%d chars, max %d)", strlen(url), sizeof(update_url) - 1);
        return false;
    }
    
    strncpy(update_url, url, sizeof(update_url) - 1);
    update_url[sizeof(update_url) - 1] = '\0';
    update_in_progress = true;
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", update_url);
    
    ESP_LOGI(TAG, "Free heap before OTA task: %lu bytes", esp_get_free_heap_size());

    // Create OTA task - stack kept minimal, buffers come from heap
    BaseType_t result = xTaskCreate(
        otaTaskWrapper,
        "ota_task",
        6144,
        this,
        5,  // Medium priority - don't starve WebSocket/WiFi tasks
        &ota_task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task (free heap: %lu)", esp_get_free_heap_size());
        update_in_progress = false;
        status->setValue(0, 0, std::string("error: task creation failed"));
        return false;
    }
    
    return true;
}

// ============================================================================
// OTA Task (runs in background)
// ============================================================================

void OtaUpdateComponent::otaTaskWrapper(void* pvParameters) {
    auto* self = static_cast<OtaUpdateComponent*>(pvParameters);
    self->otaTask();
    self->ota_task_handle = nullptr;
    self->update_in_progress = false;
    vTaskDelete(nullptr);
}

void OtaUpdateComponent::otaTask() {
    status->setValue(0, 0, std::string("connecting"));
    progress->setValue(0, 0, 0);
    
    // ---- Find the target OTA partition ----
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available! Check partition table.");
        status->setValue(0, 0, std::string("error: no OTA partition"));
        return;
    }
    
    ESP_LOGI(TAG, "Target partition: %s (addr=0x%lx, size=0x%lx)",
             update_partition->label, update_partition->address, update_partition->size);
    
    ESP_LOGI(TAG, "Free heap before HTTP init: %lu bytes", esp_get_free_heap_size());

    // ---- Set up HTTP client ----
    esp_http_client_config_t http_config = {};
    http_config.url = update_url;
    http_config.timeout_ms = 15000;
    http_config.buffer_size = 512;            // HTTP client internal buffer (small to save heap)
    http_config.buffer_size_tx = 512;         // HTTP client TX buffer
    http_config.keep_alive_enable = true;
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        status->setValue(0, 0, std::string("error: HTTP init failed"));
        return;
    }
    
    // ---- Open HTTP connection ----
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
        status->setValue(0, 0, std::string("error: connection failed"));
        esp_http_client_cleanup(client);
        return;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP server returned %d", status_code);
        char err_msg[48];
        snprintf(err_msg, sizeof(err_msg), "error: HTTP %d", status_code);
        status->setValue(0, 0, std::string(err_msg));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    
    ESP_LOGI(TAG, "Firmware size: %d bytes (partition capacity: %lu bytes)",
             content_length, update_partition->size);
    
    // ---- Validate size ----
    if (content_length > 0 && (size_t)content_length > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large! (%d bytes > %lu byte partition)",
                 content_length, update_partition->size);
        status->setValue(0, 0, std::string("error: firmware too large"));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    
    // ---- Begin OTA write ----
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        status->setValue(0, 0, std::string("error: OTA begin failed"));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    
    status->setValue(0, 0, std::string("downloading"));
    
    // ---- Download and write in chunks ----
    char* recv_buf = (char*)malloc(OTA_RECV_BUF_SIZE);
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d byte receive buffer (free heap: %lu)",
                 OTA_RECV_BUF_SIZE, esp_get_free_heap_size());
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        status->setValue(0, 0, std::string("error: out of memory"));
        return;
    }
    
    int total_read = 0;
    int last_progress_pct = -1;
    bool download_ok = true;
    
    while (true) {
        int read_len = esp_http_client_read(client, recv_buf, OTA_RECV_BUF_SIZE);
        
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error at %d bytes", total_read);
            status->setValue(0, 0, std::string("error: download failed"));
            download_ok = false;
            break;
        }
        
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;  // Download complete
            } else {
                ESP_LOGE(TAG, "Connection closed prematurely at %d bytes", total_read);
                status->setValue(0, 0, std::string("error: incomplete download"));
                download_ok = false;
                break;
            }
        }
        
        // Write chunk to OTA partition
        err = esp_ota_write(ota_handle, recv_buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at offset %d: %s", total_read, esp_err_to_name(err));
            status->setValue(0, 0, std::string("error: write failed"));
            download_ok = false;
            break;
        }
        
        total_read += read_len;
        
        // Update progress percentage
        if (content_length > 0) {
            int pct = (total_read * 100) / content_length;
            if (pct != last_progress_pct) {
                last_progress_pct = pct;
                progress->setValue(0, 0, pct);
                
                // Log every 10%
                if (pct % 10 == 0) {
                    ESP_LOGI(TAG, "Download progress: %d%% (%d / %d bytes)", pct, total_read, content_length);
                }
            }
        }
        
        // Yield to other tasks periodically
        taskYIELD();
    }
    
    free(recv_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (!download_ok) {
        esp_ota_abort(ota_handle);
        return;
    }
    
    ESP_LOGI(TAG, "Download complete: %d bytes received", total_read);
    status->setValue(0, 0, std::string("verifying"));
    progress->setValue(0, 0, 100);
    
    // ---- Finalize OTA (validates image header, checksums, etc.) ----
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Firmware image validation FAILED (corrupt or incompatible binary)");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        }
        status->setValue(0, 0, std::string("error: validation failed"));
        return;
    }
    
    // ---- Set new boot partition ----
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        status->setValue(0, 0, std::string("error: set boot failed"));
        return;
    }
    
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "OTA UPDATE SUCCESSFUL!");
    ESP_LOGI(TAG, "New firmware written to: %s", update_partition->label);
    ESP_LOGI(TAG, "Rebooting in 2 seconds...");
    ESP_LOGI(TAG, "============================================");
    
    status->setValue(0, 0, std::string("rebooting"));
    
    // Brief delay so the "rebooting" status can be sent over WebSocket
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    esp_restart();
    // Never reaches here
}
