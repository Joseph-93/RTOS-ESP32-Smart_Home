// WebSocket client for ESP32 communication
class ESP32WebSocket {
    constructor(host) {
        this.host = host;
        this.ws = null;
        this.connected = false;
        this.messageId = 0;
        this.pendingRequests = new Map();
        this.reconnectTimer = null;
    }

    connect() {
        return new Promise((resolve, reject) => {
            console.log(`Connecting to WebSocket: ws://${this.host}/ws`);
            
            this.ws = new WebSocket(`ws://${this.host}/ws`);
            
            this.ws.onopen = () => {
                console.log('WebSocket connected');
                this.connected = true;
                if (this.reconnectTimer) {
                    clearTimeout(this.reconnectTimer);
                    this.reconnectTimer = null;
                }
                resolve();
            };
            
            this.ws.onmessage = (event) => {
                console.log('WebSocket received:', event.data);
                try {
                    const response = JSON.parse(event.data);
                    
                    // If response has an id, it's a reply to a request
                    if (response.id !== undefined && this.pendingRequests.has(response.id)) {
                        const { resolve, reject } = this.pendingRequests.get(response.id);
                        this.pendingRequests.delete(response.id);
                        
                        if (response.error) {
                            reject(new Error(response.error));
                        } else {
                            resolve(response);
                        }
                    } else {
                        // Unsolicited message from server (push notification)
                        this.handlePushMessage(response);
                    }
                } catch (error) {
                    console.error('Failed to parse WebSocket message:', error);
                }
            };
            
            this.ws.onerror = (error) => {
                console.error('WebSocket error:', error);
                this.connected = false;
                reject(error);
            };
            
            this.ws.onclose = () => {
                console.log('WebSocket closed');
                this.connected = false;
                
                // Auto-reconnect after 2 seconds
                if (!this.reconnectTimer) {
                    this.reconnectTimer = setTimeout(() => {
                        console.log('Attempting to reconnect...');
                        this.connect().catch(err => console.error('Reconnect failed:', err));
                    }, 2000);
                }
            };
        });
    }

    send(message) {
        return new Promise((resolve, reject) => {
            if (!this.connected || !this.ws) {
                reject(new Error('WebSocket not connected'));
                return;
            }
            
            // Add unique ID to track response
            const id = this.messageId++;
            message.id = id;
            
            // Store promise callbacks
            this.pendingRequests.set(id, { resolve, reject });
            
            // Set timeout for request
            setTimeout(() => {
                if (this.pendingRequests.has(id)) {
                    this.pendingRequests.delete(id);
                    reject(new Error('Request timeout'));
                }
            }, 10000); // 10 second timeout
            
            const messageStr = JSON.stringify(message);
            console.log('WebSocket sending:', messageStr);
            this.ws.send(messageStr);
        });
    }

    handlePushMessage(message) {
        // Handle unsolicited messages from ESP32 (parameter updates, etc.)
        console.log('Push message from ESP32:', message);
        
        // Dispatch custom event that UI can listen to
        window.dispatchEvent(new CustomEvent('esp32-push', { detail: message }));
    }

    async getComponents() {
        const response = await this.send({ type: 'get_components' });
        return response.components || [];
    }

    async getParamInfo(comp, param_type, idx = -1) {
        const response = await this.send({
            type: 'get_param_info',
            comp,
            param_type,
            idx
        });
        return response;
    }

    async getParam(comp, param_type, idx, row, col) {
        const response = await this.send({
            type: 'get_param',
            comp,
            param_type,
            idx,
            row,
            col
        });
        return response.value;
    }

    async setParam(comp, param_type, idx, row, col, value) {
        const response = await this.send({
            type: 'set_param',
            comp,
            param_type,
            idx,
            row,
            col,
            value
        });
        return response.success;
    }

    async invokeAction(comp, action) {
        const response = await this.send({
            type: 'invoke_action',
            comp,
            action
        });
        return response.success;
    }

    close() {
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.ws) {
            this.ws.close();
        }
    }
}

// Global WebSocket instance
let esp32ws = null;

// Initialize WebSocket connection
async function initWebSocket(host) {
    if (esp32ws) {
        esp32ws.close();
    }
    
    esp32ws = new ESP32WebSocket(host);
    await esp32ws.connect();
    return esp32ws;
}
