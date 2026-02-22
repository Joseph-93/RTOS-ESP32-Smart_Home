/**
 * ESP32 Component Detail View
 * Handles dynamic loading of component parameters via WebSocket with pagination
 */

let currentDevice = null;
let currentComponent = null;
let activeSubscriptions = [];  // Track active subscriptions for cleanup

// Pagination state
let allParams = [];  // All parameters loaded from device
let currentPage = 1;
let pageSize = 25;
let totalPages = 1;

// Loading helpers
function showPageLoading(text = 'Loading...') {
    const overlay = document.getElementById('page-loading');
    if (overlay) {
        const textEl = overlay.querySelector('.loading-text');
        if (textEl) textEl.textContent = text;
        overlay.classList.remove('hidden');
    }
}

function hidePageLoading() {
    const overlay = document.getElementById('page-loading');
    if (overlay) {
        overlay.classList.add('hidden');
    }
}

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

async function initComponent(deviceName, componentName) {
    console.log('[JS] initComponent called for:', deviceName, componentName);
    hideError();
    showPageLoading(`Connecting to ${deviceName}...`);
    
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
        hidePageLoading();
        return;
    }
    
    showPageLoading(`Loading ${componentName}...`);
    console.log('[JS] Loading parameters...');
    await loadParameters(deviceName, componentName);
    console.log('[JS] Done loading component');
    hidePageLoading();
}

// Cleanup subscriptions when leaving page
window.addEventListener('beforeunload', async () => {
    console.log('[JS] Cleaning up subscriptions');
    for (const sub of activeSubscriptions) {
        try {
            // Use new ID-based unsubscribe
            await esp32ws.unsubscribeById(sub.param_id, sub.row, sub.col);
        } catch (error) {
            console.error('Error unsubscribing:', error);
        }
    }
});

function handleParameterUpdate(event) {
    const data = event.detail;
    console.log('[WS Push] Parameter update:', data);
    if (data.type !== 'param_update') return;
    
    // New API uses param_id - extract from push message
    const { param_id, row, col, value } = data;
    
    // Build input ID based on param_id
    const inputId = `param_${param_id}_${row}_${col}`;
    
    console.log('[WS Push] Looking for input with ID:', inputId);
    
    // Try to find the input element - could be various types
    const element = document.getElementById(inputId);
    const slider = document.getElementById(inputId + '_slider');
    
    if (element) {
        // Check if it's a bool-buttons container (div with class bool-buttons)
        if (element.classList.contains('bool-buttons')) {
            const buttons = element.querySelectorAll('.bool-btn');
            const isTrue = (value === true || value === 'true' || value === 1);
            console.log('[WS Push] Bool update - isTrue:', isTrue, 'value:', value);
            if (buttons.length >= 2) {
                buttons[0].classList.toggle('active', isTrue);
                buttons[1].classList.toggle('active', !isTrue);
            }
        } else if (element.tagName === 'TEXTAREA') {
            if (element !== document.activeElement) {
                element.value = value;
            }
        } else {
            // Number input
            if (element !== document.activeElement) {
                element.value = value;
            }
            if (slider && slider !== document.activeElement) {
                slider.value = value;
            }
        }
    } else {
        console.log('[WS Push] Element not found for ID:', inputId);
    }
    
    // No notification for push updates - they happen frequently and would spam the user
}

async function loadParameters(deviceName, componentName) {
    const container = document.getElementById('params-container');
    
    try {
        // Get COUNTS first via WebSocket - one type at a time
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
        
        // Collect all parameter entries as a flat list of [row][col] entries
        allParams = [];
        
        // Fetch each parameter and expand to individual entries
        for (let i = 0; i < intCount; i++) {
            console.log(`[JS] Fetching int param ${i}...`);
            const param = await esp32ws.getParamInfo(componentName, 'int', i);
            // Expand to individual row/col entries
            for (let r = 0; r < param.rows; r++) {
                for (let c = 0; c < param.cols; c++) {
                    allParams.push({ ...param, row: r, col: c, category: 'int' });
                }
            }
            await new Promise(r => setTimeout(r, 100));
        }
        
        for (let i = 0; i < floatCount; i++) {
            console.log(`[JS] Fetching float param ${i}...`);
            const param = await esp32ws.getParamInfo(componentName, 'float', i);
            for (let r = 0; r < param.rows; r++) {
                for (let c = 0; c < param.cols; c++) {
                    allParams.push({ ...param, row: r, col: c, category: 'float' });
                }
            }
            await new Promise(r => setTimeout(r, 100));
        }
        
        for (let i = 0; i < boolCount; i++) {
            console.log(`[JS] Fetching bool param ${i}...`);
            const param = await esp32ws.getParamInfo(componentName, 'bool', i);
            for (let r = 0; r < param.rows; r++) {
                for (let c = 0; c < param.cols; c++) {
                    allParams.push({ ...param, row: r, col: c, category: 'bool' });
                }
            }
            await new Promise(r => setTimeout(r, 100));
        }
        
        for (let i = 0; i < strCount; i++) {
            console.log(`[JS] Fetching string param ${i}...`);
            const param = await esp32ws.getParamInfo(componentName, 'str', i);
            for (let r = 0; r < param.rows; r++) {
                for (let c = 0; c < param.cols; c++) {
                    allParams.push({ ...param, row: r, col: c, category: 'str' });
                }
            }
            await new Promise(r => setTimeout(r, 100));
        }
        
        if (allParams.length === 0) {
            container.innerHTML = '<p class="empty-state">No parameters available</p>';
            document.getElementById('bottom-pagination').style.display = 'none';
            return;
        }
        
        // Calculate total pages and render first page
        totalPages = Math.ceil(allParams.length / pageSize);
        currentPage = 1;
        await renderCurrentPage();
        
    } catch (error) {
        console.error('[JS] Error loading parameters:', error);
        showError('Failed to load parameters', error.message);
        container.innerHTML = `<p class="error-text">Error: ${error.message}</p>`;
    }
}

// Render the current page of parameters
async function renderCurrentPage() {
    const container = document.getElementById('params-container');
    container.innerHTML = '<div class="loading">Loading page...</div>';
    
    // Unsubscribe from previous page's subscriptions
    for (const sub of activeSubscriptions) {
        try {
            await esp32ws.unsubscribeById(sub.param_id, sub.row, sub.col);
        } catch (error) {
            console.error('Error unsubscribing:', error);
        }
    }
    activeSubscriptions = [];
    
    // Calculate page slice
    const startIdx = (currentPage - 1) * pageSize;
    const endIdx = Math.min(startIdx + pageSize, allParams.length);
    const pageParams = allParams.slice(startIdx, endIdx);
    
    let html = '';
    
    for (const entry of pageParams) {
        html += await createParamEntryById(currentDevice, currentComponent, entry);
    }
    
    container.innerHTML = html || '<p class="empty-state">No parameters on this page</p>';
    
    // Update pagination controls
    updatePaginationControls();
}

// Update pagination button states and info
function updatePaginationControls() {
    const prevBtn = document.getElementById('prevPage');
    const nextBtn = document.getElementById('nextPage');
    const pageInfo = document.getElementById('pageInfo');
    const prevBtnBottom = document.getElementById('prevPageBottom');
    const nextBtnBottom = document.getElementById('nextPageBottom');
    const pageInfoBottom = document.getElementById('pageInfoBottom');
    
    const infoText = `Page ${currentPage} of ${totalPages} (${allParams.length} entries)`;
    
    if (prevBtn) prevBtn.disabled = currentPage <= 1;
    if (nextBtn) nextBtn.disabled = currentPage >= totalPages;
    if (pageInfo) pageInfo.textContent = infoText;
    
    if (prevBtnBottom) prevBtnBottom.disabled = currentPage <= 1;
    if (nextBtnBottom) nextBtnBottom.disabled = currentPage >= totalPages;
    if (pageInfoBottom) pageInfoBottom.textContent = infoText;
}

// Pagination navigation functions
async function nextPage() {
    if (currentPage < totalPages) {
        currentPage++;
        await renderCurrentPage();
        window.scrollTo(0, 0);
    }
}

async function prevPage() {
    if (currentPage > 1) {
        currentPage--;
        await renderCurrentPage();
        window.scrollTo(0, 0);
    }
}

async function changePageSize(newSize) {
    pageSize = parseInt(newSize);
    totalPages = Math.ceil(allParams.length / pageSize);
    currentPage = 1;  // Reset to first page
    await renderCurrentPage();
}

// Create a single param entry (one [row][col] value)
async function createParamEntryById(deviceName, component, entry) {
    const { name, param_id: paramId, type, rows, cols, min, max, readOnly, row: r, col: c } = entry;
    
    // Type badge for visual identification
    const typeIcons = { int: '📊', float: '📈', bool: '🔘', str: '📝' };
    const typeIcon = typeIcons[type] || '📋';
    
    const value = await getParamValueById(paramId, r, c);
    const inputId = `param_${paramId}_${r}_${c}`;
    
    // Add read-only class if parameter is read-only
    const readOnlyClass = readOnly ? ' read-only' : '';
    const disabledAttr = readOnly ? ' disabled' : '';
    const readOnlyLabel = readOnly ? ' <span class="read-only-badge">🔒</span>' : '';
    
    // Show row/col only for multi-dimensional params
    const dimLabel = (rows > 1 || cols > 1) ? `[${r}][${c}]` : '';
    
    let html = `<div class="param-item${readOnlyClass}">`;
    html += `<label><span class="type-icon">${typeIcon}</span> <strong>${name}${dimLabel}:</strong>${readOnlyLabel}</label>`;
    html += '<div class="param-control">';
    
    if (type === 'bool') {
        const isTrue = (value === 'true' || value === true);
        html += `<div class="bool-buttons" id="${inputId}">`;
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
    
    return html;
}

// Get param value by ID and subscribe for updates
async function getParamValueById(paramId, row, col) {
    try {
        // Subscribe to get initial value and future updates
        const value = await esp32ws.subscribeById(paramId, row, col);
        
        // Track subscription for cleanup
        activeSubscriptions.push({ param_id: paramId, row, col });
        
        // Small delay to avoid overwhelming ESP32
        await new Promise(r => setTimeout(r, 50));
        
        return value !== null ? value : '';
    } catch (error) {
        console.error('Error subscribing to param by ID:', error);
        return '';
    }
}

// Sync functions for slider/number inputs
function syncNumberInput(inputId, value) {
    document.getElementById(inputId).value = value;
}

function syncSlider(sliderId, value) {
    document.getElementById(sliderId).value = value;
}

// New ID-based param setter
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

// Save string param by ID
function saveStringParamById(paramId, row, col, inputId) {
    const value = document.getElementById(inputId).value;
    setParamValueById(paramId, row, col, value);
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
