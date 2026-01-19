/**
 * DEBUG - Logs exact HTTP requests and responses WITH BYTE COUNTS
 */

let debugCounter = 0;

function getByteSize(str) {
    return new Blob([str]).size;
}

// Intercept ALL fetch calls
(function() {
    const originalFetch = window.fetch;
    window.fetch = async function(...args) {
        const [url, options = {}] = args;
        
        debugCounter++;
        const requestNum = debugCounter;
        
        // Build the exact HTTP request text
        const method = options.method || 'GET';
        const headers = options.headers || {};
        const body = options.body || '';
        
        const urlObj = new URL(url);
        
        let requestText = `${method} ${urlObj.pathname}${urlObj.search} HTTP/1.1\r\n`;
        requestText += `Host: ${urlObj.host}\r\n`;
        
        // Add custom headers
        for (const [key, value] of Object.entries(headers)) {
            requestText += `${key}: ${value}\r\n`;
        }
        
        requestText += `\r\n`;
        
        if (body) {
            requestText += body;
        }
        
        const requestBytes = getByteSize(requestText);
        const browserExtraBytes = 200; // Approximate User-Agent, Accept, etc.
        const estimatedTotalBytes = requestBytes + browserExtraBytes;
        
        console.log(`\n========== HTTP REQUEST #${requestNum} ==========`);
        console.log(`CUSTOM HEADERS + BODY: ${requestBytes} bytes`);
        console.log(`BROWSER WILL ADD: ~${browserExtraBytes} bytes (User-Agent, Accept, etc.)`);
        console.log(`ESTIMATED TOTAL ESP32 RECEIVES: ~${estimatedTotalBytes} bytes`);
        console.log(`\n${requestText}`);
        console.log(`==================================================\n`);
        
        // Execute original fetch
        const response = await originalFetch.apply(this, args);
        
        // Log response
        const clonedResponse = response.clone();
        const responseBody = await clonedResponse.text();
        
        let responseText = `HTTP/1.1 ${response.status} ${response.statusText}\r\n`;
        response.headers.forEach((value, key) => {
            responseText += `${key}: ${value}\r\n`;
        });
        responseText += `\r\n`;
        responseText += responseBody;
        
        const responseBytes = getByteSize(responseText);
        
        console.log(`\n========== HTTP RESPONSE #${requestNum} ==========`);
        console.log(`SIZE: ${responseBytes} bytes`);
        console.log(`\n${responseText}`);
        console.log(`==================================================\n`);
        
        return response;
    };
})();

// WebSocket message size tracking
const originalWebSocket = window.WebSocket;
window.WebSocket = function(...args) {
    const ws = new originalWebSocket(...args);
    
    const originalSend = ws.send;
    ws.send = function(data) {
        const size = getByteSize(data);
        const wsFrameOverhead = 6; // WebSocket frame header
        const totalSize = size + wsFrameOverhead;
        
        console.log(`\n========== WEBSOCKET SEND ==========`);
        console.log(`PAYLOAD: ${size} bytes`);
        console.log(`WS FRAME OVERHEAD: ${wsFrameOverhead} bytes`);
        console.log(`TOTAL ESP32 RECEIVES: ${totalSize} bytes`);
        console.log(`\nDATA: ${data}`);
        console.log(`====================================\n`);
        
        return originalSend.call(this, data);
    };
    
    return ws;
};

