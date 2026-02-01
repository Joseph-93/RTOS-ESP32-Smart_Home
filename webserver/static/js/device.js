/**
 * ESP32 Device Interface
 * Handles dynamic loading of components and parameters via WebSocket
 */

// Global device info for onclick handlers
let currentDeviceInfo = null;

function showError(message, details = null) {
    const banner = document.getElementById('error-banner');
    if (banner) {
        let html = `<strong>⚠️ Error:</strong> ${message}`;
        if (details) {
            html += `<br><small>${JSON.stringify(details, null, 2)}</small>`;
        }
        banner.innerHTML = html;
        banner.style.display = 'block';
        setTimeout(() => { banner.style.display = 'none'; }, 10000);
    }
}

function hideError() {
    const banner = document.getElementById('error-banner');
    if (banner) banner.style.display = 'none';
}

async function initDevice(deviceName) {
    console.log('[JS] initDevice called for:', deviceName);
    try {
        // Get ESP32 connection info from Django
        const deviceInfoResponse = await fetch(`/api/${deviceName}/info/`);
        currentDeviceInfo = await deviceInfoResponse.json();
        
        console.log('[JS] Device info:', currentDeviceInfo);
        
        // Initialize WebSocket connection
        await initWebSocket(currentDeviceInfo.host);
        console.log('[JS] WebSocket connected');
        
        // Get components via WebSocket
        console.log('[JS] Getting components...');
        const components = await esp32ws.getComponents();
        console.log('[JS] Components:', components);
        
        if (components && components.length > 0) {
            // Extract component names (new API returns {name, id} objects)
            const componentNames = components.map(c => typeof c === 'string' ? c : c.name);
            console.log('[JS] Found', componentNames.length, 'components');
            await displayComponents(componentNames);
        } else {
            console.error('[JS] No components in response');
            document.getElementById('components-container').innerHTML = 
                '<div class="empty-state"><h3>No components found</h3></div>';
        }
    } catch (error) {
        console.error('[JS] Error loading device:', error);
        showError('Failed to load device', error.message);
        document.getElementById('components-container').innerHTML = 
            `<div class="empty-state"><h3>Error loading device</h3><p>${error.message}</p><p>Check browser console (F12) for details</p></div>`;
    }
}

async function displayComponents(componentNames) {
    const container = document.getElementById('components-container');
    container.innerHTML = '<div class="component-list"></div>';
    const list = container.querySelector('.component-list');
    
    hideError();
    
    // Load each component's parameters
    for (const componentName of componentNames) {
        const card = await createComponentCard(componentName);
        list.appendChild(card);
        // Small delay between components to avoid overwhelming the ESP32
        await new Promise(r => setTimeout(r, 100));
    }
}

async function createComponentCard(componentName) {
    const card = document.createElement('div');
    card.className = 'component-card';
    card.innerHTML = `
        <h3>${componentName}</h3>
        <div class="loading">Loading parameters...</div>
    `;
    
    try {
        // Fetch parameter counts and info ONE AT A TIME to avoid overwhelming ESP32
        console.log(`[JS] Getting param counts for ${componentName}...`);
        
        const intCount = (await esp32ws.getParamInfo(componentName, 'int', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        const floatCount = (await esp32ws.getParamInfo(componentName, 'float', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        const boolCount = (await esp32ws.getParamInfo(componentName, 'bool', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        const strCount = (await esp32ws.getParamInfo(componentName, 'str', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        const triggerCount = (await esp32ws.getParamInfo(componentName, 'trigger', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        // Fetch each param ONE AT A TIME
        const intParams = [];
        for (let i = 0; i < intCount; i++) {
            intParams.push(await esp32ws.getParamInfo(componentName, 'int', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const floatParams = [];
        for (let i = 0; i < floatCount; i++) {
            floatParams.push(await esp32ws.getParamInfo(componentName, 'float', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const boolParams = [];
        for (let i = 0; i < boolCount; i++) {
            boolParams.push(await esp32ws.getParamInfo(componentName, 'bool', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const stringParams = [];
        for (let i = 0; i < strCount; i++) {
            stringParams.push(await esp32ws.getParamInfo(componentName, 'str', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const triggers = [];
        for (let i = 0; i < triggerCount; i++) {
            triggers.push(await esp32ws.getParamInfo(componentName, 'trigger', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        // Build parameter UI
        let html = '';
        
        // Int parameters
        if (intParams.length > 0) {
            html += '<div class="param-group"><h4>Integer Parameters</h4>';
            for (const param of intParams) {
                html += await createParamSectionNew(componentName, param);
            }
            html += '</div>';
        }
        
        // Float parameters
        if (floatParams.length > 0) {
            html += '<div class="param-group"><h4>Float Parameters</h4>';
            for (const param of floatParams) {
                html += await createParamSectionNew(componentName, param);
            }
            html += '</div>';
        }
        
        // Bool parameters
        if (boolParams.length > 0) {
            html += '<div class="param-group"><h4>Boolean Parameters</h4>';
            for (const param of boolParams) {
                html += await createParamSectionNew(componentName, param);
            }
            html += '</div>';
        }
        
        // String parameters
        if (stringParams.length > 0) {
            html += '<div class="param-group"><h4>String Parameters</h4>';
            for (const param of stringParams) {
                html += await createParamSectionNew(componentName, param);
            }
            html += '</div>';
        }
        
        // Triggers
        if (triggers.length > 0) {
            html += '<div class="actions-group">';
            for (const trigger of triggers) {
                html += `<button class="action-btn" onclick="invokeTriggerById(${trigger.param_id})">⚡ ${trigger.name}</button>`;
            }
            html += '</div>';
        }
        
        card.querySelector('.loading').remove();
        card.innerHTML += html;
        
    } catch (error) {
        console.error(`Error loading ${componentName}:`, error);
        card.innerHTML = `<h3>${componentName}</h3><p>Error loading: ${error.message}</p>`;
    }
    
    return card;
}

// New param section using param_id
async function createParamSectionNew(component, param) {
    const { name, param_id: paramId, type, rows, cols, min, max, readOnly } = param;
    
    let html = '';
    
    for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
            const value = await getParamValueById(paramId, r, c);
            const inputId = `param_${paramId}_${r}_${c}`;
            
            const readOnlyClass = readOnly ? ' read-only' : '';
            const disabledAttr = readOnly ? ' disabled' : '';
            
            html += `<div class="param-item${readOnlyClass}">`;
            html += `<label><strong>${name}[${r}][${c}]:</strong></label>`;
            html += '<div class="param-control">';
            
            if (type === 'bool') {
                const isTrue = (value === 'true' || value === true);
                html += '<div class="bool-buttons">';
                if (readOnly) {
                    html += `<button class="bool-btn ${isTrue ? 'active' : ''}" disabled>True</button>`;
                    html += `<button class="bool-btn ${!isTrue ? 'active' : ''}" disabled>False</button>`;
                } else {
                    html += `<button class="bool-btn ${isTrue ? 'active' : ''}" onclick="setParamValueById(${paramId}, ${r}, ${c}, true); this.classList.add('active'); this.nextElementSibling.classList.remove('active');">True</button>`;
                    html += `<button class="bool-btn ${!isTrue ? 'active' : ''}" onclick="setParamValueById(${paramId}, ${r}, ${c}, false); this.classList.add('active'); this.previousElementSibling.classList.remove('active');">False</button>`;
                }
                html += '</div>';
            } else if (type === 'str') {
                html += `<textarea id="${inputId}"${disabledAttr}>${escapeHtml(value)}</textarea>`;
                if (!readOnly) {
                    html += `<button class="save-btn" onclick="saveStringParamById(${paramId}, ${r}, ${c}, '${inputId}')">💾 Save</button>`;
                }
            } else if (type === 'int' || type === 'float') {
                const step = type === 'float' ? '0.01' : '1';
                const minVal = min !== undefined ? min : 0;
                const maxVal = max !== undefined ? max : 100;
                html += '<div class="number-control">';
                if (readOnly) {
                    html += `<input type="range" id="${inputId}_slider" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" disabled>`;
                    html += `<input type="number" id="${inputId}" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" disabled>`;
                } else {
                    html += `<input type="range" id="${inputId}_slider" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncNumberInput('${inputId}', this.value)" onchange="setParamValueById(${paramId}, ${r}, ${c}, this.value)">`;
                    html += `<input type="number" id="${inputId}" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncSlider('${inputId}_slider', this.value)" onchange="setParamValueById(${paramId}, ${r}, ${c}, this.value)">`;
                }
                html += '</div>';
            }
            
            html += '</div></div>';
        }
    }
    
    return html;
}

// Get param value by ID via WebSocket
async function getParamValueById(paramId, row, col) {
    try {
        const data = await esp32ws.getParamById(paramId, row, col);
        // Small delay to avoid overwhelming ESP32
        await new Promise(r => setTimeout(r, 50));
        return data.value !== null ? data.value : '';
    } catch (error) {
        console.error('Error getting param value:', error);
        return '';
    }
}

function syncNumberInput(inputId, value) {
    document.getElementById(inputId).value = value;
}

function syncSlider(sliderId, value) {
    document.getElementById(sliderId).value = value;
}

function saveStringParamById(paramId, row, col, inputId) {
    const value = document.getElementById(inputId).value;
    setParamValueById(paramId, row, col, value);
}

async function setParamValueById(paramId, row, col, value) {
    try {
        const success = await esp32ws.setParamById(paramId, row, col, value);
        
        if (success) {
            console.log('Parameter updated successfully');
            notifications.success('Parameter updated');
        } else {
            notifications.error('Failed to update parameter');
        }
    } catch (error) {
        console.error('Error setting param value:', error);
        notifications.error('Error updating parameter');
    }
}

async function invokeTriggerById(paramId) {
    try {
        const success = await esp32ws.invokeTriggerById(paramId, 0, 0, '');
        
        if (success) {
            notifications.success('Trigger executed successfully');
        } else {
            notifications.error('Failed to execute trigger');
        }
    } catch (error) {
        console.error('Error invoking trigger:', error);
        notifications.error('Error executing trigger');
    }
}

function escapeHtml(text) {
    if (!text) return '';
    const map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    };
    return String(text).replace(/[&<>"']/g, m => map[m]);
}

// Cleanup when leaving page
window.addEventListener('beforeunload', () => {
    console.log('[JS] Cleaning up WebSocket connection');
    if (esp32ws) {
        esp32ws.close();
    }
});
