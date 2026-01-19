/**
 * ESP32 Device Interface
 * Handles dynamic loading of components and parameters
 */

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
        const deviceInfo = await deviceInfoResponse.json();
        
        const esp32BaseUrl = `http://${deviceInfo.host}:${deviceInfo.port}`;
        console.log('[JS] ESP32 Base URL:', esp32BaseUrl);
        
        // Load components directly from ESP32
        console.log('[JS] Fetching components from ESP32:', `${esp32BaseUrl}/api/components`);
        const response = await fetch(`${esp32BaseUrl}/api/components`);
        console.log('[JS] Response status:', response.status);
        const data = await response.json();
        console.log('[JS] Received data:', data);
        
        if (data.components && data.components.length > 0) {
            console.log('[JS] Found', data.components.length, 'components');
            displayComponents(deviceInfo, data.components);
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

async function displayComponents(deviceInfo, components) {
    const container = document.getElementById('components-container');
    container.innerHTML = '<div class="component-list"></div>';
    const list = container.querySelector('.component-list');
    
    hideError();
    
    // Load each component's parameters
    for (const component of components) {
        const card = await createComponentCard(deviceInfo, component);
        list.appendChild(card);
    }
}

async function createComponentCard(deviceInfo, componentName) {
    const esp32BaseUrl = `http://${deviceInfo.host}:${deviceInfo.port}`;
    const card = document.createElement('div');
    card.className = 'component-card';
    card.innerHTML = `
        <h3>${componentName}</h3>
        <div class="loading">Loading parameters...</div>
    `;
    
    try {
        // Get parameter info from ESP32 directly - ONE TYPE AT A TIME
        const data = {};
        
        console.log('[JS] Fetching int params...');
        let response = await fetch(`${esp32BaseUrl}/api/param_info?comp=${componentName}&type=int`);
        data.int_params = (await response.json()).params || [];
        
        console.log('[JS] Fetching float params...');
        response = await fetch(`${esp32BaseUrl}/api/param_info?comp=${componentName}&type=float`);
        data.float_params = (await response.json()).params || [];
        
        console.log('[JS] Fetching bool params...');
        response = await fetch(`${esp32BaseUrl}/api/param_info?comp=${componentName}&type=bool`);
        data.bool_params = (await response.json()).params || [];
        
        console.log('[JS] Fetching string params...');
        response = await fetch(`${esp32BaseUrl}/api/param_info?comp=${componentName}&type=str`);
        data.string_params = (await response.json()).params || [];
        
        console.log('[JS] Fetching actions...');
        response = await fetch(`${esp32BaseUrl}/api/param_info?comp=${componentName}&type=actions`);
        data.actions = (await response.json()).actions || [];
        
        if (data.error) {
            showError(`Component ${componentName}: ${data.error}`);
            card.innerHTML = `<h3>${componentName}</h3><p class="error-text">Error: ${data.error}</p>`;
            return card;
        }
        
        // Build parameter UI
        let html = '';
        
        // Int parameters
        if (data.int_params && data.int_params.length > 0) {
            html += '<div class="param-group"><h4>Integer Parameters</h4>';
            for (let i = 0; i < data.int_params.length; i++) {
                const param = data.int_params[i];
                html += await createParamSection(deviceName, componentName, 'int', i, param);
            }
            html += '</div>';
        }
        
        // Float parameters
        if (data.float_params && data.float_params.length > 0) {
            html += '<div class="param-group"><h4>Float Parameters</h4>';
            for (let i = 0; i < data.float_params.length; i++) {
                const param = data.float_params[i];
                html += await createParamSection(deviceName, componentName, 'float', i, param);
            }
            html += '</div>';
        }
        
        // Bool parameters
        if (data.bool_params && data.bool_params.length > 0) {
            html += '<div class="param-group"><h4>Boolean Parameters</h4>';
            for (let i = 0; i < data.bool_params.length; i++) {
                const param = data.bool_params[i];
                html += await createParamSection(deviceName, componentName, 'bool', i, param);
            }
            html += '</div>';
        }
        
        // String parameters
        if (data.string_params && data.string_params.length > 0) {
            html += '<div class="param-group"><h4>String Parameters</h4>';
            for (let i = 0; i < data.string_params.length; i++) {
                const param = data.string_params[i];
                html += await createParamSection(deviceName, componentName, 'str', i, param);
            }
            html += '</div>';
        }
        
        // Actions
        if (data.actions && data.actions.length > 0) {
            html += '<div class="actions-group">';
            for (const action of data.actions) {
                html += `<button class="action-btn" onclick="invokeAction('${deviceName}', '${componentName}', '${action}')">${action}</button>`;
            }
            html += '</div>';
        }
        
        card.querySelector('.loading').remove();
        card.innerHTML += html;
        
    } catch (error) {
        card.innerHTML = `<h3>${componentName}</h3><p>Error loading: ${error.message}</p>`;
    }
    
    return card;
}

async function createParamSection(deviceName, component, type, idx, param) {
    const { name, rows, cols, min, max } = param;
    
    let html = '';
    
    for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
            const value = await getParamValue(deviceName, component, type, idx, r, c);
            const inputId = `${component}_${type}_${idx}_${r}_${c}`;
            
            html += '<div class="param-item">';
            html += `<label><strong>${name}[${r}][${c}]:</strong></label>`;
            html += '<div class="param-control">';
            
            if (type === 'bool') {
                const isTrue = (value === 'true' || value === true);
                html += '<div class="bool-buttons">';
                html += `<button class="bool-btn ${isTrue ? 'active' : ''}" onclick="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, true); this.classList.add('active'); this.nextElementSibling.classList.remove('active');">True</button>`;
                html += `<button class="bool-btn ${!isTrue ? 'active' : ''}" onclick="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, false); this.classList.add('active'); this.previousElementSibling.classList.remove('active');">False</button>`;
                html += '</div>';
            } else if (type === 'str') {
                html += `<textarea id="${inputId}">${escapeHtml(value)}</textarea>`;
                html += `<button class="save-btn" onclick="saveStringParam('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, '${inputId}')">💾 Save</button>`;
            } else if (type === 'int' || type === 'float') {
                const step = type === 'float' ? '0.01' : '1';
                const minVal = min !== undefined ? min : 0;
                const maxVal = max !== undefined ? max : 100;
                html += '<div class="number-control">';
                html += `<input type="range" id="${inputId}_slider" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncNumberInput('${inputId}', this.value)" onchange="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, this.value)">`;
                html += `<input type="number" id="${inputId}" min="${minVal}" max="${maxVal}" step="${step}" value="${value}" oninput="syncSlider('${inputId}_slider', this.value)" onchange="setParamValue('${deviceName}', '${component}', '${type}', ${idx}, ${r}, ${c}, this.value)">`;
                html += '</div>';
            }
            
            html += '</div></div>';
        }
    }
    
    return html;
}

async function getParamValue(deviceName, comp, type, idx, row, col) {
    try {
        const response = await fetch(`/api/${deviceName}/get_param/?comp=${comp}&type=${type}&idx=${idx}&row=${row}&col=${col}`);
        const data = await response.json();
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

function saveStringParam(deviceName, comp, type, idx, row, col, inputId) {
    const value = document.getElementById(inputId).value;
    setParamValue(deviceName, comp, type, idx, row, col, value);
}

async function setParamValue(deviceName, comp, type, idx, row, col, value) {
    try {
        const response = await fetch(`/api/${deviceName}/set_param/`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'X-CSRFToken': getCookie('csrftoken')
            },
            body: JSON.stringify({ comp, type, idx, row, col, value })
        });
        
        const data = await response.json();
        if (data.success) {
            console.log('Parameter updated successfully');
        } else {
            notifications.error('Failed to update parameter');
        }
    } catch (error) {
        console.error('Error setting param value:', error);
        notifications.error('Error updating parameter');
    }
}

async function invokeAction(deviceName, comp, action) {
    try {
        const response = await fetch(`/api/${deviceName}/invoke_action/`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'X-CSRFToken': getCookie('csrftoken')
            },
            body: JSON.stringify({ comp, action })
        });
        
        const data = await response.json();
        if (data.success) {
            notifications.success(`Action "${action}" executed successfully`);
        } else {
            notifications.error('Failed to execute action');
        }
    } catch (error) {
        console.error('Error invoking action:', error);
        notifications.error('Error executing action');
    }
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

function escapeHtml(text) {
    const map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    };
    return text.replace(/[&<>"']/g, m => map[m]);
}
