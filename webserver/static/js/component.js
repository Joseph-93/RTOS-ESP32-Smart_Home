/**
 * ESP32 Component Detail View
 * Handles dynamic loading of component actions and parameters via WebSocket
 */

let currentDevice = null;
let currentComponent = null;
let activeSubscriptions = [];  // Track active subscriptions for cleanup

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

function toggleAccordion(btn) {
    const content = btn.nextElementSibling;
    const icon = btn.querySelector('.accordion-icon');
    const isOpen = content.classList.contains('open');
    
    if (isOpen) {
        content.classList.remove('open');
        icon.textContent = '▼';
    } else {
        content.classList.add('open');
        icon.textContent = '▲';
    }
}

async function initComponent(deviceName, componentName) {
    console.log('[JS] initComponent called for:', deviceName, componentName);
    hideError();
    
    currentDevice = deviceName;
    currentComponent = componentName;
    activeSubscriptions = [];  // Reset subscriptions
    
    // Initialize WebSocket connection
    const esp32Host = window.esp32Host;
    try {
        await initWebSocket(esp32Host);
        console.log('[JS] WebSocket connected');
        
        // Set up parameter update handler
        window.addEventListener('esp32-push', handleParameterUpdate);
        
    } catch (error) {
        console.error('[JS] WebSocket connection failed:', error);
        showError('Failed to connect to ESP32 WebSocket', error.message);
        return;
    }
    
    console.log('[JS] Loading actions...');
    await loadActions(deviceName, componentName);
    await new Promise(r => setTimeout(r, 100)); // WAIT 100ms before loading params
    
    console.log('[JS] Loading parameters...');
    await loadParameters(deviceName, componentName);
    console.log('[JS] Done loading component');
}

// Cleanup subscriptions when leaving page
window.addEventListener('beforeunload', async () => {
    console.log('[JS] Cleaning up subscriptions');
    for (const sub of activeSubscriptions) {
        try {
            await esp32ws.unsubscribe(sub.comp, sub.param_type, sub.idx, sub.row, sub.col);
        } catch (error) {
            console.error('Error unsubscribing:', error);
        }
    }
});

function handleParameterUpdate(event) {
    const data = event.detail;
    console.log('[WS Push] Parameter update:', data);
    if (data.type !== 'param_update') return;
    if (data.comp !== currentComponent) return;
    
    const { comp, param_type, idx, row, col, value } = data;
    const inputId = `${comp}_${param_type}_${idx}_${row}_${col}`;
    
    console.log('[WS Push] Looking for input with ID:', inputId);
    
    // Update UI based on parameter type
    if (param_type === 'bool') {
        const trueBtn = document.querySelector(`button[onclick*="setParamValue('${currentDevice}', '${comp}', '${param_type}', ${idx}, ${row}, ${col}, true)"]`);
        const falseBtn = document.querySelector(`button[onclick*="setParamValue('${currentDevice}', '${comp}', '${param_type}', ${idx}, ${row}, ${col}, false)"]`);
        
        if (trueBtn && falseBtn) {
            if (value === true || value === 'true') {
                trueBtn.classList.add('active');
                falseBtn.classList.remove('active');
            } else {
                trueBtn.classList.remove('active');
                falseBtn.classList.add('active');
            }
        }
    } else if (param_type === 'str') {
        const textarea = document.getElementById(inputId);
        if (textarea && textarea !== document.activeElement) {
            textarea.value = value;
        }
    } else if (param_type === 'int' || param_type === 'float') {
        const input = document.getElementById(inputId);
        const slider = document.getElementById(inputId + '_slider');
        
        if (input && input !== document.activeElement) {
            input.value = value;
        }
        if (slider && slider !== document.activeElement) {
            slider.value = value;
        }
    }
    
    // No notification for push updates - they happen frequently and would spam the user
}

async function loadActions(deviceName, componentName) {
    const container = document.getElementById('actions-container');
    
    try {
        // Get count first via WebSocket
        console.log('[JS] Getting action count...');
        const countData = await esp32ws.getParamInfo(componentName, 'actions', -1);
        const actionCount = countData.count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        if (actionCount === 0) {
            container.innerHTML = '<p class="empty-state">No actions available</p>';
            return;
        }
        
        // Fetch each action ONE AT A TIME
        const actions = [];
        for (let i = 0; i < actionCount; i++) {
            console.log(`[JS] Fetching action ${i}...`);
            const data = await esp32ws.getParamInfo(componentName, 'actions', i);
            if (data.name) actions.push(data.name);
            await new Promise(r => setTimeout(r, 100));
        }
        
        let html = '<div class="actions-grid">';
        for (const action of actions) {
            html += `<button class="action-btn" onclick="invokeAction('${deviceName}', '${componentName}', '${action}')">
                        ⚡ ${action}
                     </button>`;
        }
        html += '</div>';
        
        container.innerHTML = html;
        
    } catch (error) {
        console.error('[JS] Error loading actions:', error);
        showError('Failed to load actions', error.message);
        container.innerHTML = `<p class="error-text">Error: ${error.message}</p>`;
    }
}

async function loadParameters(deviceName, componentName) {
    const container = document.getElementById('params-container');
    
    try {
        // Get COUNTS first via WebSocket
        console.log('[JS] Getting int param count...');
        const intCount = (await esp32ws.getParamInfo(componentName, 'int', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        console.log('[JS] Getting float param count...');
        const floatCount = (await esp32ws.getParamInfo(componentName, 'float', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        console.log('[JS] Getting bool param count...');
        const boolCount = (await esp32ws.getParamInfo(componentName, 'bool', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        console.log('[JS] Getting string param count...');
        const strCount = (await esp32ws.getParamInfo(componentName, 'str', -1)).count || 0;
        await new Promise(r => setTimeout(r, 100));
        
        // Now fetch each parameter ONE AT A TIME
        const intParams = [];
        for (let i = 0; i < intCount; i++) {
            console.log(`[JS] Fetching int param ${i}...`);
            intParams.push(await esp32ws.getParamInfo(componentName, 'int', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const floatParams = [];
        for (let i = 0; i < floatCount; i++) {
            console.log(`[JS] Fetching float param ${i}...`);
            floatParams.push(await esp32ws.getParamInfo(componentName, 'float', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const boolParams = [];
        for (let i = 0; i < boolCount; i++) {
            console.log(`[JS] Fetching bool param ${i}...`);
            boolParams.push(await esp32ws.getParamInfo(componentName, 'bool', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        const stringParams = [];
        for (let i = 0; i < strCount; i++) {
            console.log(`[JS] Fetching string param ${i}...`);
            stringParams.push(await esp32ws.getParamInfo(componentName, 'str', i));
            await new Promise(r => setTimeout(r, 100));
        }
        
        if (intParams.length === 0 && floatParams.length === 0 && 
            boolParams.length === 0 && stringParams.length === 0) {
            container.innerHTML = '<p class="empty-state">No parameters available</p>';
            return;
        }
        
        let html = '';
        
        // Integer parameters
        if (intParams.length > 0) {
            html += '<div class="param-group"><h4>📊 Integer Parameters</h4>';
            for (let i = 0; i < intParams.length; i++) {
                html += await createParamSection(deviceName, componentName, 'int', i, intParams[i]);
            }
            html += '</div>';
        }
        
        // Float parameters
        if (floatParams.length > 0) {
            html += '<div class="param-group"><h4>📈 Float Parameters</h4>';
            for (let i = 0; i < floatParams.length; i++) {
                html += await createParamSection(deviceName, componentName, 'float', i, floatParams[i]);
            }
            html += '</div>';
        }
        
        // Boolean parameters
        if (boolParams.length > 0) {
            html += '<div class="param-group"><h4>🔘 Boolean Parameters</h4>';
            for (let i = 0; i < boolParams.length; i++) {
                html += await createParamSection(deviceName, componentName, 'bool', i, boolParams[i]);
            }
            html += '</div>';
        }
        
        // String parameters
        if (stringParams.length > 0) {
            html += '<div class="param-group"><h4>📝 String Parameters</h4>';
            for (let i = 0; i < stringParams.length; i++) {
                html += await createParamSection(deviceName, componentName, 'str', i, stringParams[i]);
            }
            html += '</div>';
        }
        
        container.innerHTML = html;
        
    } catch (error) {
        console.error('[JS] Error loading parameters:', error);
        showError('Failed to load parameters', error.message);
        container.innerHTML = `<p class="error-text">Error: ${error.message}</p>`;
    }
}

async function createParamSection(deviceName, component, type, idx, param) {
    const { name, rows, cols, min, max, readOnly } = param;
    
    let html = '';
    
    for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
            const value = await getParamValue(deviceName, component, type, idx, r, c);
            const inputId = `${component}_${type}_${idx}_${r}_${c}`;
            
            // Add read-only class if parameter is read-only
            const readOnlyClass = readOnly ? ' read-only' : '';
            const disabledAttr = readOnly ? ' disabled' : '';
            const readOnlyLabel = readOnly ? ' <span class="read-only-badge">🔒</span>' : '';
            
            html += `<div class="param-item${readOnlyClass}">`;
            html += `<label><strong>${name}[${r}][${c}]:</strong>${readOnlyLabel}</label>`;
            html += '<div class="param-control">';
            
            if (type === 'bool') {
                const isTrue = (value === 'true' || value === true);
                html += '<div class="bool-buttons">';
                if (readOnly) {
                    html += `<button class="bool-btn ${isTrue ? 'active' : ''}" disabled>True</button>`;
                    html += `<button class="bool-btn ${!isTrue ? 'active' : ''}" disabled>False</button>`;
                } else {
                    html += `<button class="bool-btn ${isTrue ? 'active' : ''}" onclick="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, true); this.classList.add('active'); this.nextElementSibling.classList.remove('active');">True</button>`;
                    html += `<button class="bool-btn ${!isTrue ? 'active' : ''}" onclick="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, false); this.classList.add('active'); this.previousElementSibling.classList.remove('active');">False</button>`;
                }
                html += '</div>';
            } else if (type === 'str') {
                html += `<textarea id="${inputId}"${disabledAttr}>${escapeHtml(value)}</textarea>`;
                if (!readOnly) {
                    html += `<button class="save-btn" onclick="saveStringParam('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, '${inputId}')">💾 Save</button>`;
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
                    html += `<input type="range" id="${inputId}_slider" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncNumberInput('${inputId}', this.value)" onchange="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, this.value)">`;
                    html += `<input type="number" id="${inputId}" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncSlider('${inputId}_slider', this.value)" onchange="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, this.value)">`;
                }
                html += '</div>';
            }
            
            html += '</div></div>';
        }
    }
    
    return html;
}

async function getParamValue(deviceName, comp, type, idx, row, col) {
    try {
        // Subscribe instead of get - will receive initial value and future updates
        const value = await esp32ws.subscribe(comp, type, idx, row, col);
        
        // Track subscription for cleanup
        activeSubscriptions.push({ comp, param_type: type, idx, row, col });
        
        return value !== null ? value : '';
    } catch (error) {
        console.error('Error subscribing to param:', error);
        return '';
    }
}

function syncNumberInput(inputId, value) {
    document.getElementById(inputId).value = value;
}

function syncSlider(sliderId, value) {
    document.getElementById(sliderId).value = value;
}

function saveStringParam(deviceName, comp, type, idx, row, col, inputId) {
    const value = document.getElementById(inputId).value;
    setParamValue(deviceName, comp, type, idx, row, col, value);
}

async function setParamValue(deviceName, comp, type, idx, row, col, value) {
    try {
        // Convert value to proper type
        let convertedValue = value;
        if (type === 'int') {
            convertedValue = parseInt(value, 10);
        } else if (type === 'float') {
            convertedValue = parseFloat(value);
        } else if (type === 'bool') {
            convertedValue = (value === true || value === 'true' || value === '1');
        }
        // str type stays as string
        
        const success = await esp32ws.setParam(comp, type, idx, row, col, convertedValue);
        
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

async function invokeAction(deviceName, comp, action) {
    if (!confirm(`Invoke action "${action}" on ${comp}?`)) {
        return;
    }
    
    try {
        const success = await esp32ws.invokeAction(comp, action);
        
        if (success) {
            notifications.success(`Action "${action}" executed successfully!`);
        } else {
            notifications.error(`Action failed`);
        }
    } catch (error) {
        console.error('Error invoking action:', error);
        notifications.error(`Error: ${error.message}`);
    }
}

function showSuccess(message) {
    const banner = document.getElementById('error-banner');
    if (banner) {
        banner.innerHTML = `<strong>✓</strong> ${message}`;
        banner.style.background = 'var(--accent-success, #2ecc71)';
        banner.style.display = 'block';
        setTimeout(() => { 
            banner.style.display = 'none';
            banner.style.background = '';
        }, 3000);
    }
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function getCookie(name) {
    let cookieValue = null;
    if (document.cookie && document.cookie !== '') {
        const cookies = document.cookie.split(';');
        for (let i = 0; i < cookies.length; i++) {
            const cookie = cookies[i].trim();
            if (cookie.substring(0, name.length + 1) === (name + '=')) {
                cookieValue = decodeURIComponent(cookie.substring(name.length + 1));
                break;
            }
        }
    }
    return cookieValue;
}
