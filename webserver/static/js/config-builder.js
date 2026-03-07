/**
 * Configuration Builder for Watcher and ActionManager
 * Provides user-friendly modals for configuring complex JSON parameters
 */

// ============================================================================
// PARAMETER PICKER - Select a parameter from any component
// ============================================================================
class ParameterPicker {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.modal = null;
        this.onSelect = null;
        this.components = [];
        this.selectedComp = null;
        this.params = [];
    }

    async show(options = {}) {
        // options: { filter: 'all'|'int'|'float'|'bool'|'str', onSelect: callback }
        return new Promise((resolve) => {
            this.onSelect = (result) => {
                this.hide();
                resolve(result);
            };
            this._createModal(options);
            this._loadComponents();
        });
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    _createModal(options) {
        const filter = options.filter || 'all';
        
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal">
                <div class="config-modal-header">
                    <h2>📍 Select Parameter</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body">
                    <div class="picker-columns">
                        <div class="picker-column">
                            <h4>Device</h4>
                            <div class="picker-list" id="picker-devices">
                                <div class="loading">Loading...</div>
                            </div>
                            <div class="picker-device-actions" style="margin-top: 8px; display: flex; gap: 4px; flex-wrap: wrap;">
                                <button class="btn btn-sm btn-secondary" id="picker-rescan-btn" title="Scan for devices via mDNS">🔍 Scan</button>
                                <input type="text" id="picker-add-ip" placeholder="10.0.0.x" style="flex: 1; min-width: 80px; padding: 4px 6px; font-size: 12px;">
                                <button class="btn btn-sm btn-primary" id="picker-add-btn" title="Add device by IP">➕</button>
                            </div>
                        </div>
                        <div class="picker-column">
                            <h4>Component</h4>
                            <div class="picker-list" id="picker-components">
                                <div class="empty-hint">Select a device</div>
                            </div>
                        </div>
                        <div class="picker-column">
                            <h4>Parameter</h4>
                            <div class="picker-list" id="picker-params">
                                <div class="empty-hint">Select a component</div>
                            </div>
                        </div>
                        <div class="picker-column picker-column-narrow">
                            <h4>Index</h4>
                            <div class="picker-indices">
                                <label>Row: <input type="number" id="picker-row" value="0" min="0" max="99"></label>
                                <label>Col: <input type="number" id="picker-col" value="0" min="0" max="99"></label>
                            </div>
                        </div>
                    </div>
                    <div class="picker-preview">
                        <strong>Selected:</strong> <code id="picker-preview-text">None</code>
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="this.closest('.config-modal-overlay').remove()">Cancel</button>
                    <button class="btn btn-primary" id="picker-select-btn" disabled>Select</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        
        // Setup event listeners
        this.modal.querySelector('#picker-select-btn').onclick = () => this._confirmSelection();
        this.modal.querySelector('#picker-row').onchange = () => this._updatePreview();
        this.modal.querySelector('#picker-col').onchange = () => this._updatePreview();
        
        // Device management buttons
        this.modal.querySelector('#picker-rescan-btn').onclick = () => this._rescanDevices();
        this.modal.querySelector('#picker-add-btn').onclick = () => this._addDeviceByIp();
        this.modal.querySelector('#picker-add-ip').onkeypress = (e) => {
            if (e.key === 'Enter') this._addDeviceByIp();
        };
    }
    
    async _rescanDevices() {
        const btn = this.modal.querySelector('#picker-rescan-btn');
        const originalText = btn.textContent;
        btn.textContent = '⏳...';
        btn.disabled = true;
        
        try {
            const result = await this.ws.send({ type: 'rescan_devices' });
            
            if (result.added && result.added.length > 0) {
                showNotification(`Found ${result.added.length} new device(s)`, 'success');
            } else if (result.discovered && result.discovered.length > 0) {
                showNotification(`Found ${result.discovered.length} device(s) (already known)`, 'info');
            } else {
                showNotification('No devices found via mDNS', 'warning');
            }
            
            // Reload device list
            await this._loadComponents();
        } catch (e) {
            console.error('Rescan failed:', e);
            showNotification('Scan failed: ' + e.message, 'error');
        } finally {
            btn.textContent = originalText;
            btn.disabled = false;
        }
    }
    
    async _addDeviceByIp() {
        const input = this.modal.querySelector('#picker-add-ip');
        const ip = input.value.trim();
        
        if (!ip) {
            showNotification('Enter an IP address', 'warning');
            return;
        }
        
        const btn = this.modal.querySelector('#picker-add-btn');
        btn.disabled = true;
        
        try {
            const result = await this.ws.send({ type: 'add_device', ip: ip });
            
            if (result.error) {
                showNotification(result.error, 'error');
            } else {
                showNotification(result.message || `Added ${ip}`, 'success');
                input.value = '';
                
                // Wait a moment for connection, then reload
                setTimeout(() => this._loadComponents(), 1500);
            }
        } catch (e) {
            console.error('Add device failed:', e);
            showNotification('Failed to add device: ' + e.message, 'error');
        } finally {
            btn.disabled = false;
        }
    }

    async _loadComponents() {
        try {
            // Get ALL devices (local + remote)
            const response = await this.ws.send({ type: 'get_all_devices' });
            this.devices = response.devices || [];
            
            const list = this.modal.querySelector('#picker-devices');
            if (this.devices.length === 0) {
                list.innerHTML = '<div class="empty-hint">No devices found</div>';
                return;
            }
            
            list.innerHTML = this.devices.map(d => `
                <div class="picker-item ${d.connected ? '' : 'disconnected'}" data-device="${d.device}">
                    ${d.device === 'self' ? '🏠' : '📡'} ${d.name}
                    ${!d.connected ? '<span class="status-offline">offline</span>' : ''}
                </div>
            `).join('');
            
            // Add click handlers
            list.querySelectorAll('.picker-item').forEach(item => {
                item.onclick = () => this._selectDevice(item.dataset.device);
            });
        } catch (e) {
            console.error('Failed to load devices:', e);
            const list = this.modal.querySelector('#picker-devices');
            list.innerHTML = '<div class="error-hint">Error loading devices</div>';
        }
    }

    _selectDevice(deviceId) {
        this.selectedDevice = this.devices.find(d => d.device === deviceId);
        this.selectedComp = null;
        this.selectedParam = null;
        
        // Highlight selection
        this.modal.querySelectorAll('#picker-devices .picker-item').forEach(item => {
            item.classList.toggle('selected', item.dataset.device === deviceId);
        });
        
        // Show components for this device
        const compList = this.modal.querySelector('#picker-components');
        const paramsList = this.modal.querySelector('#picker-params');
        paramsList.innerHTML = '<div class="empty-hint">Select a component</div>';
        
        if (!this.selectedDevice.components || this.selectedDevice.components.length === 0) {
            compList.innerHTML = '<div class="empty-hint">No components</div>';
            return;
        }
        
        compList.innerHTML = this.selectedDevice.components.map(c => `
            <div class="picker-item" data-comp="${c.name}">${c.name}</div>
        `).join('');
        
        compList.querySelectorAll('.picker-item').forEach(item => {
            item.onclick = () => this._selectComponent(item.dataset.comp);
        });
        
        this._updatePreview();
    }

    async _selectComponent(compName) {
        this.selectedComp = compName;
        this.selectedParam = null;
        
        // Highlight selection
        this.modal.querySelectorAll('#picker-components .picker-item').forEach(item => {
            item.classList.toggle('selected', item.dataset.comp === compName);
        });
        
        // Load parameters for this device/component
        const paramsList = this.modal.querySelector('#picker-params');
        paramsList.innerHTML = '<div class="loading">Loading...</div>';
        
        try {
            // Use get_device_component_params for both local and remote
            const response = await this.ws.send({
                type: 'get_device_component_params',
                device: this.selectedDevice.device,
                comp: compName
            });
            
            this.params = response.params || [];
            
            if (this.params.length === 0) {
                paramsList.innerHTML = '<div class="empty-hint">No parameters</div>';
                return;
            }
            
            const typeIcons = { int: '📊', float: '📈', bool: '🔘', str: '📝' };
            paramsList.innerHTML = this.params.map((p, i) => {
                // Handle both 'type' and 'param_type' field names
                const pType = p.type || p.param_type;
                return `
                    <div class="picker-item" data-idx="${i}">
                        <span class="type-badge type-${pType}">${typeIcons[pType] || '📦'}</span>
                        ${p.name}
                        <span class="dim-info">[${p.rows}×${p.cols}]</span>
                    </div>
                `;
            }).join('');
            
            paramsList.querySelectorAll('.picker-item').forEach(item => {
                item.onclick = () => this._selectParam(parseInt(item.dataset.idx));
            });
        } catch (e) {
            console.error('Failed to load params:', e);
            paramsList.innerHTML = '<div class="error-hint">Error loading parameters</div>';
        }
    }

    _selectParam(idx) {
        this.selectedParam = this.params[idx];
        
        // Highlight
        this.modal.querySelectorAll('#picker-params .picker-item').forEach((item, i) => {
            item.classList.toggle('selected', i === idx);
        });
        
        // Update row/col max values
        const rowInput = this.modal.querySelector('#picker-row');
        const colInput = this.modal.querySelector('#picker-col');
        rowInput.max = this.selectedParam.rows - 1;
        colInput.max = this.selectedParam.cols - 1;
        rowInput.value = Math.min(rowInput.value, this.selectedParam.rows - 1);
        colInput.value = Math.min(colInput.value, this.selectedParam.cols - 1);
        
        this._updatePreview();
    }

    _updatePreview() {
        const preview = this.modal.querySelector('#picker-preview-text');
        const selectBtn = this.modal.querySelector('#picker-select-btn');
        
        if (!this.selectedParam || !this.selectedDevice) {
            preview.textContent = 'None';
            selectBtn.disabled = true;
            return;
        }
        
        const row = parseInt(this.modal.querySelector('#picker-row').value) || 0;
        const col = parseInt(this.modal.querySelector('#picker-col').value) || 0;
        const deviceLabel = this.selectedDevice.device === 'self' ? '' : `${this.selectedDevice.device}:`;
        
        preview.textContent = `${deviceLabel}${this.selectedComp}.${this.selectedParam.name}[${row}][${col}]`;
        selectBtn.disabled = false;
    }

    _confirmSelection() {
        const row = parseInt(this.modal.querySelector('#picker-row').value) || 0;
        const col = parseInt(this.modal.querySelector('#picker-col').value) || 0;
        
        // Handle both 'type' and 'param_type', and 'id' and 'param_id' field names
        const paramType = this.selectedParam.type || this.selectedParam.param_type;
        const paramId = this.selectedParam.id || this.selectedParam.param_id;
        const deviceLabel = this.selectedDevice.device === 'self' ? '' : `${this.selectedDevice.device}:`;
        
        this.onSelect({
            device: this.selectedDevice.device,
            comp: this.selectedComp,
            param: this.selectedParam.name,
            param_id: paramId,
            param_type: paramType,
            row,
            col,
            expression: `${deviceLabel}${this.selectedComp}.${this.selectedParam.name}[${row}][${col}]`
        });
    }
}


// ============================================================================
// EXPRESSION BUILDER - Build boolean expressions for Watcher
// ============================================================================
class ExpressionBuilder {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.picker = new ParameterPicker(wsConnection);
        this.modal = null;
        this.conditions = [];
        this.variables = {};  // Map of variable name -> definition
    }

    async show(existingExpression = '', existingVariables = {}) {
        this.variables = { ...existingVariables };  // Copy existing variables
        return new Promise((resolve) => {
            this.onComplete = (result) => {
                this.hide();
                resolve(result);
            };
            this._createModal(existingExpression);
        });
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    _createModal(existingExpression) {
        this.conditions = [];
        
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal config-modal-wide">
                <div class="config-modal-header">
                    <h2>🔍 Expression Builder</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body">
                    <p class="helper-text">Build a boolean expression that triggers when it becomes true or false.</p>
                    
                    <div class="conditions-list" id="conditions-list">
                        <div class="empty-hint">Click "Add Condition" to start building your expression</div>
                    </div>
                    
                    <button class="btn btn-secondary" id="add-condition-btn">➕ Add Condition</button>
                    
                    <div class="expression-preview">
                        <strong>Expression:</strong>
                        <code id="expression-preview-text">${existingExpression || '(empty)'}</code>
                    </div>
                    
                    <div class="variables-preview" style="margin-top: 10px; padding: 10px; background: #f5f5f5; border-radius: 4px;">
                        <strong>Variables:</strong>
                        <div id="variables-list" style="font-family: monospace; font-size: 12px; margin-top: 5px;">
                            ${this._renderVariablesList()}
                        </div>
                    </div>
                    
                    <div class="manual-entry">
                        <label>Or enter expression manually (use variable names from above):</label>
                        <input type="text" id="manual-expression" value="${existingExpression}" placeholder="e.g., button_pressed == true">
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="this.closest('.config-modal-overlay').remove()">Cancel</button>
                    <button class="btn btn-primary" id="expression-save-btn">Save Expression</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        
        this.modal.querySelector('#add-condition-btn').onclick = () => this._addCondition();
        this.modal.querySelector('#expression-save-btn').onclick = () => this._saveExpression();
        this.modal.querySelector('#manual-expression').oninput = (e) => {
            this.modal.querySelector('#expression-preview-text').textContent = e.target.value || '(empty)';
        };
        
        // Parse existing expression if provided
        if (existingExpression) {
            this._parseExistingExpression(existingExpression);
        }
    }

    _renderVariablesList() {
        const vars = Object.entries(this.variables);
        if (vars.length === 0) {
            return '<em>No variables defined yet</em>';
        }
        return vars.map(([name, def]) => {
            const device = def.device === 'self' ? 'local' : def.device;
            return `<div>• <strong>${name}</strong> → ${device}/${def.component}.${def.param}[${def.row}][${def.col}]</div>`;
        }).join('');
    }

    _generateVariableName(param) {
        // Generate a clean variable name from the parameter info
        // e.g., "button_0_pressed" or "motion_detected"
        let baseName = param.param.toLowerCase().replace(/[^a-z0-9]/g, '_');
        
        // Add device prefix for remote devices
        if (param.device !== 'self') {
            const deviceShort = param.device.split('.').pop();  // Last octet of IP
            baseName = `dev${deviceShort}_${baseName}`;
        }
        
        // Ensure unique name
        let name = baseName;
        let counter = 1;
        while (this.variables[name]) {
            name = `${baseName}_${counter}`;
            counter++;
        }
        
        return name;
    }

    async _addCondition() {
        const param = await this.picker.show();
        if (!param) return;
        
        // Show variable name input modal
        const nameModal = document.createElement('div');
        nameModal.className = 'config-modal-overlay';
        const suggestedName = this._generateVariableName(param);
        nameModal.innerHTML = `
            <div class="config-modal" style="max-width: 450px;">
                <div class="config-modal-header">
                    <h2>📝 Name This Variable</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body">
                    <p style="margin-bottom: 10px;">You selected: <code>${param.expression}</code></p>
                    <p style="margin-bottom: 10px; color: #666;">Choose a short, descriptive name for this variable to use in expressions.</p>
                    <label style="display: block;">
                        <strong>Variable Name:</strong>
                        <input type="text" id="var-name-input" value="${suggestedName}" 
                               style="width: 100%; padding: 10px; margin-top: 5px; font-size: 16px; font-family: monospace;"
                               placeholder="e.g., button_pressed, motion_detected">
                    </label>
                    <p id="var-name-error" style="color: red; margin-top: 5px; display: none;"></p>
                    <p style="margin-top: 10px; font-size: 12px; color: #888;">
                        💡 Use lowercase letters, numbers, and underscores. No spaces.
                    </p>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" id="var-name-cancel">Cancel</button>
                    <button class="btn btn-primary" id="var-name-confirm">Add Variable</button>
                </div>
            </div>
        `;
        document.body.appendChild(nameModal);
        
        const input = nameModal.querySelector('#var-name-input');
        const errorP = nameModal.querySelector('#var-name-error');
        const confirmBtn = nameModal.querySelector('#var-name-confirm');
        const cancelBtn = nameModal.querySelector('#var-name-cancel');
        
        // Focus and select the input
        input.focus();
        input.select();
        
        // Validate on input
        input.oninput = () => {
            const name = input.value.trim();
            if (!name) {
                errorP.textContent = 'Name is required';
                errorP.style.display = 'block';
                confirmBtn.disabled = true;
            } else if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(name)) {
                errorP.textContent = 'Invalid name. Use letters, numbers, underscores. Must start with letter or underscore.';
                errorP.style.display = 'block';
                confirmBtn.disabled = true;
            } else if (this.variables[name]) {
                errorP.textContent = `Variable "${name}" already exists. Choose a different name.`;
                errorP.style.display = 'block';
                confirmBtn.disabled = true;
            } else {
                errorP.style.display = 'none';
                confirmBtn.disabled = false;
            }
        };
        
        // Enter key to confirm
        input.onkeydown = (e) => {
            if (e.key === 'Enter' && !confirmBtn.disabled) {
                confirmBtn.click();
            }
        };
        
        cancelBtn.onclick = () => nameModal.remove();
        
        confirmBtn.onclick = () => {
            const varName = input.value.trim();
            nameModal.remove();
            
            // Store the variable definition
            this.variables[varName] = {
                device: param.device,
                component: param.comp,
                param: param.param,
                row: param.row,
                col: param.col
            };
            
            const condition = {
                id: Date.now(),
                param,
                varName,
                operator: '==',
                value: param.param_type === 'bool' ? 'true' : '0',
                logic: this.conditions.length > 0 ? 'AND' : null
            };
            
            this.conditions.push(condition);
            this._renderConditions();
        };
    }

    _renderConditions() {
        const list = this.modal.querySelector('#conditions-list');
        
        if (this.conditions.length === 0) {
            list.innerHTML = '<div class="empty-hint">Click "Add Condition" to start building your expression</div>';
            this._updatePreview();
            return;
        }
        
        list.innerHTML = this.conditions.map((c, i) => `
            <div class="condition-row" data-id="${c.id}">
                ${i > 0 ? `
                    <select class="logic-select" data-field="logic">
                        <option value="AND" ${c.logic === 'AND' ? 'selected' : ''}>AND</option>
                        <option value="OR" ${c.logic === 'OR' ? 'selected' : ''}>OR</option>
                    </select>
                ` : '<span class="logic-placeholder">IF</span>'}
                <span class="condition-param" title="${c.param.expression}"><strong>${c.varName}</strong></span>
                <select class="operator-select" data-field="operator">
                    <option value="==" ${c.operator === '==' ? 'selected' : ''}>=</option>
                    <option value="!=" ${c.operator === '!=' ? 'selected' : ''}>≠</option>
                    <option value=">" ${c.operator === '>' ? 'selected' : ''}>&gt;</option>
                    <option value="<" ${c.operator === '<' ? 'selected' : ''}>&lt;</option>
                    <option value=">=" ${c.operator === '>=' ? 'selected' : ''}>≥</option>
                    <option value="<=" ${c.operator === '<=' ? 'selected' : ''}>≤</option>
                </select>
                <input type="text" class="value-input" data-field="value" value="${c.value}">
                <button class="btn-icon delete-condition" title="Remove">🗑️</button>
            </div>
        `).join('');
        
        // Update variables list display
        this.modal.querySelector('#variables-list').innerHTML = this._renderVariablesList();
        
        // Event listeners
        list.querySelectorAll('.condition-row').forEach(row => {
            const id = parseInt(row.dataset.id);
            const condition = this.conditions.find(c => c.id === id);
            
            row.querySelectorAll('select, input').forEach(el => {
                el.onchange = el.oninput = () => {
                    condition[el.dataset.field] = el.value;
                    this._updatePreview();
                };
            });
            
            row.querySelector('.delete-condition').onclick = () => {
                // Remove variable when condition is deleted
                delete this.variables[condition.varName];
                this.conditions = this.conditions.filter(c => c.id !== id);
                // Reset first condition's logic
                if (this.conditions.length > 0) {
                    this.conditions[0].logic = null;
                }
                this._renderConditions();
            };
        });
        
        this._updatePreview();
    }

    _updatePreview() {
        const preview = this.modal.querySelector('#expression-preview-text');
        const manualInput = this.modal.querySelector('#manual-expression');
        
        if (this.conditions.length === 0) {
            preview.textContent = '(empty)';
            manualInput.value = '';
            return;
        }
        
        // Use variable names in the expression, not raw paths
        const expr = this.conditions.map((c, i) => {
            const part = `${c.varName} ${c.operator} ${c.value}`;
            return i === 0 ? part : `${c.logic.toLowerCase()} ${part}`;
        }).join(' ');
        
        preview.textContent = expr;
        manualInput.value = expr;
    }

    _parseExistingExpression(expr) {
        // Try to parse the expression back into visual conditions
        // Pattern: varName operator value [logic varName operator value]...
        // Example: "button_pressed == true and motion_detected == false"
        
        if (!expr) return;
        
        // Split by 'and' or 'or' (case insensitive)
        const parts = expr.split(/\s+(and|or)\s+/i);
        
        let currentLogic = null;
        
        for (let i = 0; i < parts.length; i++) {
            const part = parts[i].trim();
            
            // Check if this is a logic operator
            if (part.toLowerCase() === 'and' || part.toLowerCase() === 'or') {
                currentLogic = part.toUpperCase();
                continue;
            }
            
            // Try to parse: varName operator value
            const match = part.match(/^(\w+)\s*(==|!=|>=|<=|>|<)\s*(.+)$/);
            if (match) {
                const varName = match[1];
                const operator = match[2];
                const value = match[3].trim();
                
                // Check if this variable exists in our variables map
                const varDef = this.variables[varName];
                if (varDef) {
                    // Reconstruct the param object for display
                    const param = {
                        device: varDef.device,
                        comp: varDef.component,
                        param: varDef.param,
                        row: varDef.row,
                        col: varDef.col,
                        expression: varDef.device === 'self' 
                            ? `${varDef.component}.${varDef.param}[${varDef.row}][${varDef.col}]`
                            : `${varDef.device}:${varDef.component}.${varDef.param}[${varDef.row}][${varDef.col}]`
                    };
                    
                    const condition = {
                        id: Date.now() + i,
                        param,
                        varName,
                        operator,
                        value,
                        logic: this.conditions.length > 0 ? (currentLogic || 'AND') : null
                    };
                    
                    this.conditions.push(condition);
                }
            }
            
            currentLogic = null;
        }
        
        // Render the parsed conditions
        if (this.conditions.length > 0) {
            this._renderConditions();
        }
    }

    _saveExpression() {
        const expr = this.modal.querySelector('#manual-expression').value.trim();
        // Return both the expression AND the variables
        this.onComplete({
            expression: expr,
            variables: this.variables
        });
    }
}


// ============================================================================
// ACTION BUILDER - Build actions for Watcher/ActionManager
// ============================================================================
class ActionBuilder {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.picker = new ParameterPicker(wsConnection);
        this.modal = null;
        this.actions = [];
        this.showDelay = false;  // For ActionManager
    }

    async show(existingActions = [], options = {}) {
        this.showDelay = options.showDelay || false;
        
        return new Promise((resolve) => {
            this.onComplete = (result) => {
                this.hide();
                resolve(result);
            };
            this._createModal(existingActions);
        });
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    _createModal(existingActions) {
        this.actions = existingActions.map(a => ({ ...a, id: Date.now() + Math.random() }));
        
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal config-modal-wide">
                <div class="config-modal-header">
                    <h2>⚡ Action Builder</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body">
                    <p class="helper-text">Define actions to execute when triggered. Actions run in sequence.</p>
                    
                    <div class="actions-list" id="actions-list">
                        <div class="empty-hint">No actions defined. Add one below.</div>
                    </div>
                    
                    <div class="action-type-buttons">
                        <button class="btn btn-secondary" id="add-set-action">➕ Set Parameter</button>
                    </div>
                    
                    <div class="json-preview">
                        <strong>JSON Preview:</strong>
                        <pre id="actions-json-preview">{}</pre>
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="this.closest('.config-modal-overlay').remove()">Cancel</button>
                    <button class="btn btn-primary" id="actions-save-btn">Save Actions</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        
        this.modal.querySelector('#add-set-action').onclick = () => this._addSetAction();
        this.modal.querySelector('#actions-save-btn').onclick = () => this._saveActions();
        
        this._renderActions();
    }

    async _addSetAction() {
        const param = await this.picker.show();
        if (!param) return;
        
        const action = {
            id: Date.now(),
            device: param.device,  // Include device!
            comp: param.comp,
            param: param.param,
            row: param.row,
            col: param.col,
            value: param.param_type === 'bool' ? true : 0,
            delay_ms: 0
        };
        
        this.actions.push(action);
        this._renderActions();
    }

    _renderActions() {
        const list = this.modal.querySelector('#actions-list');
        
        if (this.actions.length === 0) {
            list.innerHTML = '<div class="empty-hint">No actions defined. Add one below.</div>';
            this._updatePreview();
            return;
        }
        
        list.innerHTML = this.actions.map((a, i) => {
            const deviceLabel = a.device === 'self' ? '' : `${a.device}:`;
            return `
                <div class="action-row" data-id="${a.id}">
                    <span class="action-number">${i + 1}.</span>
                    <span class="action-type-badge action-type-set">SET</span>
                    <span class="action-target">${deviceLabel}${a.comp}.${a.param}[${a.row}][${a.col}]</span>
                    <span class="action-equals">=</span>
                    <input type="text" class="action-value" data-field="value" value="${a.value}">
                    ${this.showDelay ? `
                        <label class="delay-label">after <input type="number" class="delay-input" data-field="delay_ms" value="${a.delay_ms}" min="0"> ms</label>
                    ` : ''}
                    <button class="btn-icon move-up" title="Move Up">⬆️</button>
                    <button class="btn-icon move-down" title="Move Down">⬇️</button>
                    <button class="btn-icon delete-action" title="Remove">🗑️</button>
                </div>
            `;
        }).join('');
        
        // Event listeners
        list.querySelectorAll('.action-row').forEach((row, idx) => {
            const id = parseFloat(row.dataset.id);
            const action = this.actions.find(a => a.id === id);
            
            row.querySelectorAll('input').forEach(el => {
                el.onchange = el.oninput = () => {
                    let val = el.value;
                    if (el.dataset.field === 'delay_ms' || el.dataset.field === 'index') {
                        val = parseInt(val) || 0;
                    } else if (el.dataset.field === 'value') {
                        // Try to parse as number or boolean
                        if (val === 'true') val = true;
                        else if (val === 'false') val = false;
                        else if (!isNaN(parseFloat(val))) val = parseFloat(val);
                    }
                    action[el.dataset.field] = val;
                    this._updatePreview();
                };
            });
            
            row.querySelector('.move-up').onclick = () => {
                if (idx > 0) {
                    [this.actions[idx - 1], this.actions[idx]] = [this.actions[idx], this.actions[idx - 1]];
                    this._renderActions();
                }
            };
            
            row.querySelector('.move-down').onclick = () => {
                if (idx < this.actions.length - 1) {
                    [this.actions[idx], this.actions[idx + 1]] = [this.actions[idx + 1], this.actions[idx]];
                    this._renderActions();
                }
            };
            
            row.querySelector('.delete-action').onclick = () => {
                this.actions = this.actions.filter(a => a.id !== id);
                this._renderActions();
            };
        });
        
        this._updatePreview();
    }

    _updatePreview() {
        const preview = this.modal.querySelector('#actions-json-preview');
        
        // Clean up for JSON (remove internal id field)
        const cleanActions = this.actions.map(a => {
            const { id, ...rest } = a;
            if (!this.showDelay) delete rest.delay_ms;
            return rest;
        });
        
        preview.textContent = JSON.stringify({ actions: cleanActions }, null, 2);
    }

    _saveActions() {
        const cleanActions = this.actions.map(a => {
            const { id, ...rest } = a;
            if (!this.showDelay) delete rest.delay_ms;
            return rest;
        });
        
        this.onComplete(cleanActions);
    }
}


// ============================================================================
// WATCH SLOT EDITOR - Complete editor for a Watcher slot
// ============================================================================
class WatchSlotEditor {
    constructor(container, components, wsConnection, onSave) {
        this.container = container;
        this.components = components;
        this.ws = wsConnection;
        this.onSave = onSave;
        this.expressionBuilder = new ExpressionBuilder(wsConnection);
        this.actionBuilder = new ActionBuilder(wsConnection);
        this.modal = null;
    }

    async show(slotIndex = 0, currentData = {}) {
        this.slotIndex = slotIndex;
        
        // Parse rising/falling actions - detect cycle mode
        const risingParsed = this._parseActionData(currentData.risingActions, currentData.risingCycle);
        const fallingParsed = this._parseActionData(currentData.fallingActions, currentData.fallingCycle);
        
        this.data = {
            expression: currentData.expression || '',
            variables: currentData.variables || {},
            // Rising actions
            risingMode: risingParsed.mode,  // 'simple' or 'cycle'
            risingActions: risingParsed.actions,  // For simple mode
            risingCycle: risingParsed.cycle,  // For cycle mode: array of action arrays
            risingCycleIndex: currentData.risingCycleIndex || 0,
            // Falling actions
            fallingMode: fallingParsed.mode,
            fallingActions: fallingParsed.actions,
            fallingCycle: fallingParsed.cycle,
            fallingCycleIndex: currentData.fallingCycleIndex || 0,
            // Timing
            holdHighSec: currentData.holdHighSec || 0,
            cooldownSec: currentData.cooldownSec || 0
        };
        this._createModal();
    }
    
    _parseActionData(actions, cycle) {
        // If cycle array is provided, use cycle mode
        if (cycle && Array.isArray(cycle) && cycle.length > 0) {
            return { mode: 'cycle', actions: [], cycle: cycle };
        }
        // Otherwise simple mode with actions array
        return { mode: 'simple', actions: actions || [], cycle: [[]] };
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    _createModal() {
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal config-modal-wide">
                <div class="config-modal-header">
                    <h2>👁️ Watch Slot ${this.slotIndex} Configuration</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body">
                    <div class="slot-selector">
                        <label>Slot Index: 
                            <input type="number" id="slot-index-input" value="${this.slotIndex}" min="0" max="49">
                        </label>
                    </div>
                    
                    <div class="watch-section">
                        <h3>📐 Expression to Watch</h3>
                        <p class="helper-text">This expression is evaluated continuously. Actions trigger when it changes between true/false.</p>
                        <div class="expression-display">
                            <code id="current-expression">${this.data.expression || '(not configured)'}</code>
                            <button class="btn btn-secondary" id="edit-expression-btn">✏️ Edit</button>
                        </div>
                        <div class="variables-display" style="margin-top: 8px; font-size: 12px; color: #666;">
                            <strong>Variables:</strong> <span id="current-variables">${this._summarizeVariables()}</span>
                        </div>
                    </div>
                    
                    <div class="watch-section">
                        <h3>📈 Rising Edge Actions <span class="edge-hint">(false → true)</span></h3>
                        <p class="helper-text">These actions run when the expression becomes true.</p>
                        ${this._renderActionSection('rising')}
                    </div>
                    
                    <div class="watch-section">
                        <h3>📉 Falling Edge Actions <span class="edge-hint">(true → false)</span></h3>
                        <p class="helper-text">These actions run when the expression becomes false.</p>
                        ${this._renderActionSection('falling')}
                    </div>
                    
                    <div class="watch-section">
                        <h3>⏱️ Timing Settings</h3>
                        <p class="helper-text">Control how long the signal stays high or low.</p>
                        <div class="timing-inputs" style="display: grid; grid-template-columns: 1fr 1fr; gap: 20px;">
                            <div>
                                <label style="display: block; margin-bottom: 5px;">
                                    <strong>Hold High (seconds):</strong>
                                    <span style="font-size: 12px; color: #888; display: block;">
                                        When TRUE, stay TRUE for at least this long
                                    </span>
                                </label>
                                <input type="number" id="hold-high-input" value="${this.data.holdHighSec}" 
                                       min="0" max="86400" step="0.1"
                                       style="width: 100%; padding: 8px; font-size: 16px;">
                            </div>
                            <div>
                                <label style="display: block; margin-bottom: 5px;">
                                    <strong>Cooldown (seconds):</strong>
                                    <span style="font-size: 12px; color: #888; display: block;">
                                        After going FALSE, can't go TRUE for this long
                                    </span>
                                </label>
                                <input type="number" id="cooldown-input" value="${this.data.cooldownSec}" 
                                       min="0" max="86400" step="0.1"
                                       style="width: 100%; padding: 8px; font-size: 16px;">
                            </div>
                        </div>
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-danger" id="clear-slot-btn">🗑️ Clear Slot</button>
                    <div class="spacer"></div>
                    <button class="btn btn-secondary" onclick="this.closest('.config-modal-overlay').remove()">Cancel</button>
                    <button class="btn btn-primary" id="save-watch-btn">💾 Save Configuration</button>
                </div>
            </div>
        `;
        
        this.container.appendChild(this.modal);
        
        // Event handlers
        this.modal.querySelector('#slot-index-input').onchange = (e) => {
            this.slotIndex = parseInt(e.target.value) || 0;
            this.modal.querySelector('h2').textContent = `👁️ Watch Slot ${this.slotIndex} Configuration`;
        };
        this.modal.querySelector('#edit-expression-btn').onclick = () => this._editExpression();
        this.modal.querySelector('#clear-slot-btn').onclick = () => this._clearSlot();
        this.modal.querySelector('#save-watch-btn').onclick = () => this._save();
        
        // Mode toggles
        this._setupModeToggle('rising');
        this._setupModeToggle('falling');
        
        // Timing input handlers
        this.modal.querySelector('#hold-high-input').onchange = (e) => {
            this.data.holdHighSec = parseFloat(e.target.value) || 0;
        };
        this.modal.querySelector('#cooldown-input').onchange = (e) => {
            this.data.cooldownSec = parseFloat(e.target.value) || 0;
        };
    }
    
    _renderActionSection(edge) {
        const mode = this.data[`${edge}Mode`];
        const actions = this.data[`${edge}Actions`];
        const cycle = this.data[`${edge}Cycle`];
        const cycleIndex = this.data[`${edge}CycleIndex`];
        
        return `
            <div class="action-mode-section" id="${edge}-action-section">
                <div class="mode-toggle">
                    <label class="mode-option ${mode === 'simple' ? 'active' : ''}">
                        <input type="radio" name="${edge}-mode" value="simple" ${mode === 'simple' ? 'checked' : ''}>
                        <span>Simple</span>
                        <small>Same actions every trigger</small>
                    </label>
                    <label class="mode-option ${mode === 'cycle' ? 'active' : ''}">
                        <input type="radio" name="${edge}-mode" value="cycle" ${mode === 'cycle' ? 'checked' : ''}>
                        <span>🔄 Cycle</span>
                        <small>Toggle/rotate through steps</small>
                    </label>
                </div>
                
                <div class="simple-mode-content" style="display: ${mode === 'simple' ? 'block' : 'none'}">
                    <div class="actions-summary" id="${edge}-actions-summary">
                        ${this._summarizeActions(actions)}
                    </div>
                    <button class="btn btn-secondary edit-${edge}-simple-btn">✏️ Edit Actions</button>
                </div>
                
                <div class="cycle-mode-content" style="display: ${mode === 'cycle' ? 'block' : 'none'}">
                    <div class="cycle-info">
                        <span class="cycle-badge">🔄 ${cycle.length} step${cycle.length !== 1 ? 's' : ''}</span>
                        <span class="cycle-index-display">Next: Step ${cycleIndex + 1}</span>
                    </div>
                    <div class="cycle-steps" id="${edge}-cycle-steps">
                        ${this._renderCycleSteps(edge, cycle)}
                    </div>
                    <button class="btn btn-secondary add-${edge}-step-btn">➕ Add Step</button>
                </div>
            </div>
        `;
    }
    
    _renderCycleSteps(edge, cycle) {
        if (!cycle || cycle.length === 0) {
            return '<div class="empty-hint">No steps defined. Add one below.</div>';
        }
        
        return cycle.map((stepActions, idx) => `
            <div class="cycle-step" data-step="${idx}">
                <div class="cycle-step-header">
                    <span class="step-number">Step ${idx + 1}</span>
                    <span class="step-action-count">${stepActions.length} action${stepActions.length !== 1 ? 's' : ''}</span>
                    <div class="step-controls">
                        <button class="btn-icon edit-step-btn" title="Edit Step" data-edge="${edge}" data-step="${idx}">✏️</button>
                        ${cycle.length > 1 ? `<button class="btn-icon delete-step-btn" title="Remove Step" data-edge="${edge}" data-step="${idx}">🗑️</button>` : ''}
                    </div>
                </div>
                <div class="step-actions-preview">
                    ${this._summarizeActions(stepActions) || '<span class="empty-hint">No actions</span>'}
                </div>
            </div>
        `).join('');
    }
    
    _setupModeToggle(edge) {
        const section = this.modal.querySelector(`#${edge}-action-section`);
        if (!section) return;
        
        // Mode radio buttons
        section.querySelectorAll(`input[name="${edge}-mode"]`).forEach(radio => {
            radio.onchange = () => {
                this.data[`${edge}Mode`] = radio.value;
                
                // Update active state on labels
                section.querySelectorAll('.mode-option').forEach(label => {
                    label.classList.toggle('active', label.querySelector('input').checked);
                });
                
                // Show/hide content
                section.querySelector('.simple-mode-content').style.display = radio.value === 'simple' ? 'block' : 'none';
                section.querySelector('.cycle-mode-content').style.display = radio.value === 'cycle' ? 'block' : 'none';
                
                // Initialize cycle with current simple actions if switching to cycle mode and empty
                if (radio.value === 'cycle' && this.data[`${edge}Cycle`].length === 0) {
                    this.data[`${edge}Cycle`] = [this.data[`${edge}Actions`].slice()];
                    this._refreshCycleSteps(edge);
                }
            };
        });
        
        // Simple mode edit button
        section.querySelector(`.edit-${edge}-simple-btn`).onclick = () => this._editActions(edge, 'simple');
        
        // Add step button
        section.querySelector(`.add-${edge}-step-btn`).onclick = () => {
            this.data[`${edge}Cycle`].push([]);
            this._refreshCycleSteps(edge);
        };
        
        // Cycle step buttons (use event delegation)
        section.querySelector(`#${edge}-cycle-steps`).onclick = (e) => {
            const editBtn = e.target.closest('.edit-step-btn');
            const deleteBtn = e.target.closest('.delete-step-btn');
            
            if (editBtn) {
                const stepIdx = parseInt(editBtn.dataset.step);
                this._editCycleStep(edge, stepIdx);
            }
            
            if (deleteBtn) {
                const stepIdx = parseInt(deleteBtn.dataset.step);
                this.data[`${edge}Cycle`].splice(stepIdx, 1);
                // Adjust cycle index if needed
                if (this.data[`${edge}CycleIndex`] >= this.data[`${edge}Cycle`].length) {
                    this.data[`${edge}CycleIndex`] = 0;
                }
                this._refreshCycleSteps(edge);
            }
        };
    }
    
    _refreshCycleSteps(edge) {
        const stepsContainer = this.modal.querySelector(`#${edge}-cycle-steps`);
        const cycle = this.data[`${edge}Cycle`];
        stepsContainer.innerHTML = this._renderCycleSteps(edge, cycle);
        
        // Update cycle info
        const cycleInfo = this.modal.querySelector(`#${edge}-action-section .cycle-info`);
        if (cycleInfo) {
            cycleInfo.innerHTML = `
                <span class="cycle-badge">🔄 ${cycle.length} step${cycle.length !== 1 ? 's' : ''}</span>
                <span class="cycle-index-display">Next: Step ${this.data[`${edge}CycleIndex`] + 1}</span>
            `;
        }
    }

    _summarizeVariables() {
        const vars = Object.entries(this.data.variables || {});
        if (vars.length === 0) {
            return '<em>none</em>';
        }
        return vars.map(([name, def]) => {
            const device = def.device === 'self' ? '' : `${def.device}:`;
            return `<code>${name}</code>`;
        }).join(', ');
    }

    _summarizeActions(actions) {
        if (!actions || actions.length === 0) {
            return '<span class="empty-hint">No actions configured</span>';
        }
        
        return actions.map((a, i) => {
            if (a.type === 'SET' || !a.type) {
                return `<div class="action-summary-item">${i + 1}. SET ${a.comp || a.component}.${a.param}[${a.row}][${a.col}] = ${a.value}</div>`;
            } else if (a.type === 'NETWORK') {
                return `<div class="action-summary-item">${i + 1}. NETWORK message #${a.index}</div>`;
            }
            return '';
        }).join('');
    }

    async _editExpression() {
        const result = await this.expressionBuilder.show(this.data.expression, this.data.variables);
        if (result !== undefined && result !== null) {
            this.data.expression = result.expression;
            this.data.variables = result.variables;
            this.modal.querySelector('#current-expression').textContent = result.expression || '(not configured)';
            this.modal.querySelector('#current-variables').innerHTML = this._summarizeVariables();
        }
    }

    async _editActions(edge, mode) {
        if (mode === 'simple') {
            const result = await this.actionBuilder.show(this.data[`${edge}Actions`]);
            if (result) {
                this.data[`${edge}Actions`] = result;
                this.modal.querySelector(`#${edge}-actions-summary`).innerHTML = this._summarizeActions(result);
            }
        }
    }
    
    async _editCycleStep(edge, stepIdx) {
        const currentActions = this.data[`${edge}Cycle`][stepIdx] || [];
        const result = await this.actionBuilder.show(currentActions);
        if (result) {
            this.data[`${edge}Cycle`][stepIdx] = result;
            this._refreshCycleSteps(edge);
        }
    }

    _clearSlot() {
        const confirmDiv = document.createElement('div');
        confirmDiv.className = 'config-modal-overlay';
        confirmDiv.innerHTML = `
            <div class="config-modal" style="max-width: 350px;">
                <div class="config-modal-header">
                    <h2>⚠️ Clear Slot?</h2>
                </div>
                <div class="config-modal-body">
                    <p>This will remove all configuration for this watch slot.</p>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" id="clear-cancel">Cancel</button>
                    <button class="btn btn-danger" id="clear-confirm">🗑️ Clear Slot</button>
                </div>
            </div>
        `;
        document.body.appendChild(confirmDiv);
        
        confirmDiv.querySelector('#clear-cancel').onclick = () => confirmDiv.remove();
        confirmDiv.querySelector('#clear-confirm').onclick = () => {
            confirmDiv.remove();
            this.data = { 
                expression: '', 
                variables: {}, 
                risingMode: 'simple',
                risingActions: [], 
                risingCycle: [[]],
                fallingMode: 'simple',
                fallingActions: [],
                fallingCycle: [[]],
                holdHighSec: 0,
                cooldownSec: 0
            };
            this._save();
        };
    }

    _save() {
        this.hide();
        if (this.onSave) {
            // Build output in correct format based on mode
            const result = {
                slotIndex: this.slotIndex,
                expression: this.data.expression,
                variables: this.data.variables,
                holdHighSec: this.data.holdHighSec,
                cooldownSec: this.data.cooldownSec,
                // Include mode info for the save handler
                risingMode: this.data.risingMode,
                fallingMode: this.data.fallingMode
            };
            
            // Rising actions
            if (this.data.risingMode === 'cycle') {
                result.risingCycle = this.data.risingCycle;
                result.risingActions = [];  // Empty for cycle mode
            } else {
                result.risingActions = this.data.risingActions;
                result.risingCycle = null;
            }
            
            // Falling actions
            if (this.data.fallingMode === 'cycle') {
                result.fallingCycle = this.data.fallingCycle;
                result.fallingActions = [];
            } else {
                result.fallingActions = this.data.fallingActions;
                result.fallingCycle = null;
            }
            
            this.onSave(result);
        }
    }
}


// ============================================================================
// ACTION MANAGER EDITOR - Queue actions with timing
// ============================================================================
class ActionManagerEditor {
    constructor(container, components, wsConnection, onSave) {
        this.container = container;
        this.components = components;
        this.ws = wsConnection;
        this.onSave = onSave;
        this.actionBuilder = new ActionBuilder(wsConnection);
    }

    async show() {
        // Show action builder with delay support
        const actions = await this.actionBuilder.show([], { showDelay: true });
        if (actions && this.onSave) {
            this.onSave(actions);
        }
    }
}


// ============================================================================
// EXPRESSION MONITOR - Live view of watch expression status
// ============================================================================
class ExpressionMonitor {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.modal = null;
        this.slotIndex = null;
        this.expression = '';
        this.variables = {};
        this.refreshInterval = null;
        this.autoRefresh = true;
    }

    async show(slotIndex) {
        this.slotIndex = slotIndex;
        
        // Load slot data
        await this._loadSlotData();
        
        // Create and show modal
        this._createModal();
        
        // Start auto-refresh
        this._startAutoRefresh();
    }

    hide() {
        this._stopAutoRefresh();
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    async _loadSlotData() {
        try {
            const [exprResp, varsResp] = await Promise.all([
                this.ws.send({ type: 'get_param', comp: 'Watcher', param: 'expressions', row: this.slotIndex, col: 0 }),
                this.ws.send({ type: 'get_param', comp: 'Watcher', param: 'variables', row: 0, col: 0 })
            ]);
            
            this.expression = exprResp.value || '';
            this.variables = varsResp.value ? JSON.parse(varsResp.value) : {};
        } catch (e) {
            console.error('Failed to load slot data:', e);
            this.expression = '(error loading)';
            this.variables = {};
        }
    }

    _createModal() {
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal expr-monitor">
                <div class="config-modal-header">
                    <h2>👁️ Expression Monitor - Slot ${this.slotIndex}</h2>
                    <button class="close-btn" onclick="expressionMonitor.hide()">✕</button>
                </div>
                <div class="config-modal-body">
                    <div class="expr-monitor-header">
                        <span>Current Status:</span>
                        <div id="expr-status" class="expr-monitor-status status-unknown">
                            ⏳ Loading...
                        </div>
                    </div>
                    
                    <div class="expr-monitor-expression">
                        <strong>Expression:</strong><br>
                        <code id="expr-text">${this.expression || '(no expression)'}</code>
                    </div>
                    
                    <div id="expr-errors" class="expr-monitor-errors" style="display: none;">
                        <h4>⚠️ Errors</h4>
                        <ul id="expr-error-list"></ul>
                    </div>
                    
                    <div class="expr-monitor-variables">
                        <h4>📊 Variable Values</h4>
                        <div id="var-list">
                            <div class="loading">Loading variable values...</div>
                        </div>
                    </div>
                    
                    <div class="expr-monitor-timing" id="timing-section" style="display: none;">
                        <h4>⏱️ Timing State</h4>
                        <div id="timing-details"></div>
                    </div>
                    
                    <div class="expr-monitor-auto-refresh">
                        <label>
                            <input type="checkbox" id="auto-refresh-check" checked>
                            Auto-refresh every 500ms
                        </label>
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="openWatchSlotEditor(${this.slotIndex})">✏️ Edit Slot</button>
                    <button class="btn btn-primary" onclick="expressionMonitor.hide()">Close</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        
        // Auto-refresh toggle
        this.modal.querySelector('#auto-refresh-check').onchange = (e) => {
            this.autoRefresh = e.target.checked;
            if (this.autoRefresh) {
                this._startAutoRefresh();
            } else {
                this._stopAutoRefresh();
            }
        };
        
        // Initial refresh
        this._refresh();
    }

    _startAutoRefresh() {
        this._stopAutoRefresh();
        if (this.autoRefresh) {
            this.refreshInterval = setInterval(() => this._refresh(), 500);
        }
    }

    _stopAutoRefresh() {
        if (this.refreshInterval) {
            clearInterval(this.refreshInterval);
            this.refreshInterval = null;
        }
    }

    async _refresh() {
        if (!this.modal) return;
        
        try {
            // Get Watcher's internal state via a special request
            const response = await this.ws.send({
                type: 'get_watcher_state',
                slot: this.slotIndex
            });
            
            // Update status
            const statusDiv = this.modal.querySelector('#expr-status');
            const errorsDiv = this.modal.querySelector('#expr-errors');
            const errorList = this.modal.querySelector('#expr-error-list');
            
            if (response.error) {
                statusDiv.className = 'expr-monitor-status status-error';
                statusDiv.innerHTML = '❌ Error';
                errorsDiv.style.display = 'block';
                errorList.innerHTML = `<li>${response.error}</li>`;
            } else if (response.result === true) {
                statusDiv.className = 'expr-monitor-status status-true';
                statusDiv.innerHTML = '✅ TRUE';
                errorsDiv.style.display = 'none';
            } else if (response.result === false) {
                statusDiv.className = 'expr-monitor-status status-false';
                statusDiv.innerHTML = '○ FALSE';
                errorsDiv.style.display = 'none';
            } else {
                statusDiv.className = 'expr-monitor-status status-unknown';
                statusDiv.innerHTML = '⏳ Unknown';
                errorsDiv.style.display = 'none';
            }
            
            // Update timing state
            const timingSection = this.modal.querySelector('#timing-section');
            const timingDetails = this.modal.querySelector('#timing-details');
            const holdHighSec = response.hold_high_sec || 0;
            const cooldownSec = response.cooldown_sec || 0;
            
            if (holdHighSec > 0 || cooldownSec > 0) {
                timingSection.style.display = 'block';
                let timingHtml = '';
                
                if (holdHighSec > 0) {
                    const inHold = response.in_hold || false;
                    const holdRemaining = response.hold_remaining_sec || 0;
                    timingHtml += `
                        <div class="timing-row ${inHold ? 'timing-active' : ''}">
                            <span class="timing-label">⬆️ Hold-High:</span>
                            <span class="timing-value">${holdHighSec}s</span>
                            ${inHold ? `<span class="timing-badge hold">HOLDING (${holdRemaining}s left)</span>` : '<span class="timing-badge inactive">Ready</span>'}
                        </div>
                    `;
                }
                
                if (cooldownSec > 0) {
                    const inCooldown = response.in_cooldown || false;
                    const cooldownRemaining = response.cooldown_remaining_sec || 0;
                    timingHtml += `
                        <div class="timing-row ${inCooldown ? 'timing-active' : ''}">
                            <span class="timing-label">❄️ Cooldown:</span>
                            <span class="timing-value">${cooldownSec}s</span>
                            ${inCooldown ? `<span class="timing-badge cooldown">COOLING (${cooldownRemaining}s left)</span>` : '<span class="timing-badge inactive">Ready</span>'}
                        </div>
                    `;
                }
                
                timingDetails.innerHTML = timingHtml;
            } else {
                timingSection.style.display = 'none';
            }
            
            // Update variable values
            const varList = this.modal.querySelector('#var-list');
            const varValues = response.variable_values || {};
            const varDefs = response.variable_definitions || this.variables;
            const varErrors = response.variable_errors || {};
            
            // Find which variables are used in this expression
            const usedVars = this._extractVariablesFromExpression(this.expression, Object.keys(varDefs));
            
            if (usedVars.length === 0) {
                varList.innerHTML = '<div class="empty-hint">No variables in this expression</div>';
            } else {
                varList.innerHTML = usedVars.map(varName => {
                    const def = varDefs[varName] || {};
                    const value = varValues[varName];
                    const error = varErrors[varName];
                    const hasValue = value !== undefined && value !== null;
                    
                    const deviceLabel = def.device === 'self' ? '🏠 local' : `📡 ${def.device}`;
                    const source = `${deviceLabel} / ${def.component || '?'}.${def.param || '?'}[${def.row || 0}][${def.col || 0}]`;
                    
                    let valueClass = 'value-missing';
                    let valueText = '❌ NO VALUE';
                    
                    if (error) {
                        // Show subscription error with details
                        valueClass = 'value-error';
                        valueText = `⚠️ ${error}`;
                    } else if (hasValue) {
                        if (typeof value === 'boolean') {
                            valueClass = 'type-bool';
                            valueText = value ? '✓ true' : '✗ false';
                        } else if (typeof value === 'number') {
                            valueClass = Number.isInteger(value) ? 'type-int' : 'type-float';
                            valueText = value.toString();
                        } else {
                            valueClass = 'type-str';
                            valueText = `"${value}"`;
                        }
                    }
                    
                    return `
                        <div class="var-row ${error ? 'var-row-error' : ''}">
                            <div>
                                <span class="var-name">${varName}</span>
                                <span class="var-source">${source}</span>
                            </div>
                            <div class="var-value">
                                <span class="var-value-badge ${valueClass}">${valueText}</span>
                            </div>
                        </div>
                    `;
                }).join('');
            }
            
        } catch (e) {
            console.error('Failed to refresh expression state:', e);
            // Don't show error on every refresh failure
        }
    }

    _extractVariablesFromExpression(expr, knownVars) {
        // Find all variable names used in the expression
        const used = [];
        for (const varName of knownVars) {
            // Use word boundary matching
            const pattern = new RegExp(`\\b${varName}\\b`);
            if (pattern.test(expr)) {
                used.push(varName);
            }
        }
        return used;
    }
}

// Global instance for easy access
let expressionMonitor = null;
let variableManager = null;
let networkActionEditor = null;


// ============================================================================
// VARIABLE MANAGER - View, edit, and delete all Watcher variables
// ============================================================================
class VariableManager {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.modal = null;
        this.variables = {};
    }

    async show() {
        await this._loadVariables();
        this._createModal();
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    async _loadVariables() {
        try {
            const resp = await this.ws.send({
                type: 'get_param',
                comp: 'Watcher',
                param: 'variables',
                row: 0,
                col: 0
            });
            this.variables = resp.value ? JSON.parse(resp.value) : {};
        } catch (e) {
            console.error('Failed to load variables:', e);
            this.variables = {};
        }
    }

    async _saveVariables() {
        try {
            await this.ws.send({
                type: 'set_param',
                comp: 'Watcher',
                param: 'variables',
                row: 0,
                col: 0,
                value: JSON.stringify(this.variables)
            });
            return true;
        } catch (e) {
            console.error('Failed to save variables:', e);
            return false;
        }
    }

    _createModal() {
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal config-modal-wide">
                <div class="config-modal-header">
                    <h2>📝 Variable Manager</h2>
                    <button class="close-btn" onclick="variableManager.hide()">✕</button>
                </div>
                <div class="config-modal-body">
                    <p class="helper-text">Manage all Watcher variables. Variables are used in expressions to track device parameters.</p>
                    
                    <div class="var-manager-list" id="var-manager-list">
                        ${this._renderVariablesList()}
                    </div>
                    
                    <button class="btn btn-secondary" id="add-variable-btn" style="margin-top: 1rem;">➕ Add Variable</button>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="variableManager.hide()">Close</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        this.modal.querySelector('#add-variable-btn').onclick = () => this._addVariable();
        this._attachRowHandlers();
    }

    _renderVariablesList() {
        const vars = Object.entries(this.variables);
        if (vars.length === 0) {
            return '<div class="empty-hint">No variables defined. Click "Add Variable" to create one.</div>';
        }
        
        return `
            <table class="var-manager-table">
                <thead>
                    <tr>
                        <th>Name</th>
                        <th>Device</th>
                        <th>Component.Param</th>
                        <th>Index [row][col]</th>
                        <th>Actions</th>
                    </tr>
                </thead>
                <tbody>
                    ${vars.map(([name, def]) => `
                        <tr data-var="${name}">
                            <td><code>${name}</code></td>
                            <td>${def.device === 'self' ? '🏠 local' : `📡 ${def.device}`}</td>
                            <td><code>${def.component}.${def.param}</code></td>
                            <td>[${def.row}][${def.col}]</td>
                            <td class="var-actions">
                                <button class="btn-icon edit-var-btn" title="Edit">✏️</button>
                                <button class="btn-icon delete-var-btn" title="Delete">🗑️</button>
                            </td>
                        </tr>
                    `).join('')}
                </tbody>
            </table>
        `;
    }

    _refreshList() {
        const listEl = this.modal.querySelector('#var-manager-list');
        listEl.innerHTML = this._renderVariablesList();
        this._attachRowHandlers();
    }

    _attachRowHandlers() {
        if (!this.modal) return;
        
        this.modal.querySelectorAll('.edit-var-btn').forEach(btn => {
            btn.onclick = (e) => {
                const row = e.target.closest('tr');
                const varName = row.dataset.var;
                this._editVariable(varName);
            };
        });
        
        this.modal.querySelectorAll('.delete-var-btn').forEach(btn => {
            btn.onclick = (e) => {
                const row = e.target.closest('tr');
                const varName = row.dataset.var;
                this._deleteVariable(varName);
            };
        });
    }

    _addVariable() {
        this._showEditDialog(null, {
            device: '',
            component: '',
            param: '',
            row: 0,
            col: 0
        });
    }

    _editVariable(varName) {
        const def = this.variables[varName];
        if (!def) return;
        this._showEditDialog(varName, def);
    }

    async _deleteVariable(varName) {
        if (!confirm(`Delete variable "${varName}"?`)) return;
        
        delete this.variables[varName];
        if (await this._saveVariables()) {
            this._refreshList();
        }
    }

    _showEditDialog(existingName, def) {
        const isNew = existingName === null;
        const dialog = document.createElement('div');
        dialog.className = 'config-modal-overlay';
        dialog.style.zIndex = '1001';  // Above variable manager
        dialog.innerHTML = `
            <div class="config-modal" style="max-width: 500px;">
                <div class="config-modal-header">
                    <h2>${isNew ? '➕ Add Variable' : '✏️ Edit Variable'}</h2>
                    <button class="close-btn" id="dialog-close">✕</button>
                </div>
                <div class="config-modal-body">
                    <div class="form-group">
                        <label>Variable Name:</label>
                        <input type="text" id="var-name" value="${existingName || ''}" placeholder="e.g., button_pressed" ${isNew ? '' : 'readonly'}>
                        ${isNew ? '' : '<small style="color: #888;">Name cannot be changed. Delete and recreate to rename.</small>'}
                    </div>
                    <div class="form-group">
                        <label>Device:</label>
                        <input type="text" id="var-device" value="${def.device}" placeholder="e.g., 10.0.0.125 or self">
                        <small style="color: #888;">IP address or "self" for local hub</small>
                    </div>
                    <div class="form-group">
                        <label>Component:</label>
                        <input type="text" id="var-component" value="${def.component}" placeholder="e.g., TouchSensor">
                    </div>
                    <div class="form-group">
                        <label>Parameter:</label>
                        <input type="text" id="var-param" value="${def.param}" placeholder="e.g., touched">
                    </div>
                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
                        <div class="form-group">
                            <label>Row:</label>
                            <input type="number" id="var-row" value="${def.row}" min="0">
                        </div>
                        <div class="form-group">
                            <label>Col:</label>
                            <input type="number" id="var-col" value="${def.col}" min="0">
                        </div>
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" id="dialog-cancel">Cancel</button>
                    <button class="btn btn-primary" id="dialog-save">💾 Save</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(dialog);
        
        dialog.querySelector('#dialog-close').onclick = () => dialog.remove();
        dialog.querySelector('#dialog-cancel').onclick = () => dialog.remove();
        dialog.querySelector('#dialog-save').onclick = async () => {
            const name = dialog.querySelector('#var-name').value.trim();
            const device = dialog.querySelector('#var-device').value.trim();
            const component = dialog.querySelector('#var-component').value.trim();
            const param = dialog.querySelector('#var-param').value.trim();
            const row = parseInt(dialog.querySelector('#var-row').value) || 0;
            const col = parseInt(dialog.querySelector('#var-col').value) || 0;
            
            // Validation
            if (!name) { alert('Variable name is required'); return; }
            if (!device) { alert('Device is required'); return; }
            if (!component) { alert('Component is required'); return; }
            if (!param) { alert('Parameter is required'); return; }
            if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(name)) {
                alert('Variable name must start with a letter/underscore and contain only letters, numbers, and underscores');
                return;
            }
            
            // Check for duplicate name when adding
            if (isNew && this.variables[name]) {
                alert(`Variable "${name}" already exists`);
                return;
            }
            
            this.variables[name] = { device, component, param, row, col };
            
            if (await this._saveVariables()) {
                dialog.remove();
                this._refreshList();
            } else {
                alert('Failed to save variable');
            }
        };
    }
}


// ============================================================================
// NETWORK ACTION EDITOR - Configure network message slots
// ============================================================================
class NetworkActionEditor {
    constructor(wsConnection) {
        this.ws = wsConnection;
        this.modal = null;
        this.slotIndex = 0;
        this.data = {};
        this.onSave = null;
    }

    async show(slotIndex = 0, currentData = {}, onSave = null) {
        this.slotIndex = slotIndex;
        this.onSave = onSave;
        this.data = {
            name: currentData.name || '',
            protocol: currentData.protocol || 'HTTP',
            host: currentData.host || '',
            port: currentData.port || 80,
            path: currentData.path || '/',
            method: currentData.method || 'GET',
            headers: currentData.headers || {},
            body: currentData.body || '',
            timeout_ms: currentData.timeout_ms || 5000,
            notes: currentData.notes || ''
        };
        this._createModal();
    }

    hide() {
        if (this.modal) {
            this.modal.remove();
            this.modal = null;
        }
    }

    _parseIP(host) {
        if (!host) return ['', '', '', ''];
        const parts = host.split('.');
        if (parts.length === 4) return parts;
        return [host, '', '', ''];
    }

    _notify(message, type = 'info') {
        // Use global showNotification if available, otherwise use alert
        if (typeof window.showNotification === 'function') {
            window.showNotification(message, type);
        } else {
            if (type === 'error') alert(message);
        }
    }

    _createModal() {
        const ipParts = this._parseIP(this.data.host);
        const isHttp = this.data.protocol === 'HTTP' || this.data.protocol === 'HTTPS';
        
        this.modal = document.createElement('div');
        this.modal.className = 'config-modal-overlay';
        this.modal.innerHTML = `
            <div class="config-modal config-modal-wide">
                <div class="config-modal-header">
                    <h2>📡 Network Action Slot ${this.slotIndex}</h2>
                    <button class="close-btn" onclick="networkActionEditor.hide()">✕</button>
                </div>
                <div class="config-modal-body net-action-editor">
                    <div class="net-action-row net-action-name-row">
                        <input type="text" id="net-name" class="net-action-name-input" 
                               value="${this._escapeHtml(this.data.name)}" 
                               placeholder="Action Name (e.g., Living Room Off)">
                        <div class="net-action-buttons">
                            <button class="btn btn-danger btn-sm" id="net-delete-btn">🗑️ Delete</button>
                            <button class="btn btn-secondary btn-sm" id="net-duplicate-btn">📋 Duplicate</button>
                        </div>
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">Protocol</label>
                        <div class="net-action-protocol-btns" id="protocol-btns">
                            <button class="protocol-btn ${this.data.protocol === 'UDP' ? 'active' : ''}" data-protocol="UDP">UDP</button>
                            <button class="protocol-btn ${this.data.protocol === 'TCP' ? 'active' : ''}" data-protocol="TCP">TCP</button>
                            <button class="protocol-btn ${this.data.protocol === 'WebSocket' ? 'active' : ''}" data-protocol="WebSocket">WebSocket</button>
                            <button class="protocol-btn ${this.data.protocol === 'HTTP' ? 'active' : ''}" data-protocol="HTTP">HTTP</button>
                            <button class="protocol-btn ${this.data.protocol === 'HTTPS' ? 'active' : ''}" data-protocol="HTTPS">HTTPS</button>
                        </div>
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">IP Address</label>
                        <div class="net-action-ip-inputs">
                            <input type="number" id="ip-0" class="ip-octet" value="${ipParts[0]}" min="0" max="255">
                            <span class="ip-dot">.</span>
                            <input type="number" id="ip-1" class="ip-octet" value="${ipParts[1]}" min="0" max="255">
                            <span class="ip-dot">.</span>
                            <input type="number" id="ip-2" class="ip-octet" value="${ipParts[2]}" min="0" max="255">
                            <span class="ip-dot">.</span>
                            <input type="number" id="ip-3" class="ip-octet" value="${ipParts[3]}" min="0" max="255">
                            <span class="ip-valid" id="ip-valid">✓</span>
                        </div>
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">Port</label>
                        <input type="number" id="net-port" class="net-action-port" value="${this.data.port}" min="1" max="65535">
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">Delay (sec)</label>
                        <input type="number" id="net-delay" class="net-action-port" value="${(this.data.timeout_ms || 5000) / 1000}" min="0" max="300" step="0.1">
                    </div>
                    
                    <div class="net-action-row http-only" style="display: ${isHttp ? 'flex' : 'none'};">
                        <label class="net-action-label">Method</label>
                        <div class="net-action-method-btns" id="method-btns">
                            <button class="method-btn ${this.data.method === 'GET' ? 'active' : ''}" data-method="GET">GET</button>
                            <button class="method-btn ${this.data.method === 'POST' ? 'active' : ''}" data-method="POST">POST</button>
                            <button class="method-btn ${this.data.method === 'PUT' ? 'active' : ''}" data-method="PUT">PUT</button>
                            <button class="method-btn ${this.data.method === 'DELETE' ? 'active' : ''}" data-method="DELETE">DELETE</button>
                        </div>
                    </div>
                    
                    <div class="net-action-row http-only" style="display: ${isHttp ? 'flex' : 'none'};">
                        <label class="net-action-label">Resource</label>
                        <div class="net-action-resource-wrapper">
                            <input type="text" id="net-path" class="net-action-resource" 
                                   value="${this._escapeHtml(this.data.path)}" placeholder="/api/endpoint">
                            <button class="btn btn-icon" id="copy-path-btn" title="Copy">📋</button>
                        </div>
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">Body</label>
                        <div class="net-action-resource-wrapper">
                            <input type="text" id="net-body" class="net-action-resource" 
                                   value="${this._escapeHtml(this.data.body)}" placeholder='{"key": "value"}'>
                            <button class="btn btn-icon" id="copy-body-btn" title="Copy">📋</button>
                        </div>
                    </div>
                    
                    <div class="net-action-row">
                        <label class="net-action-label">Notes</label>
                        <input type="text" id="net-notes" class="net-action-resource" 
                               value="${this._escapeHtml(this.data.notes || '')}" placeholder="Optional notes about this action">
                    </div>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="networkActionEditor.hide()">Cancel</button>
                    <button class="btn btn-success" id="net-test-btn">🧪 Test</button>
                    <button class="btn btn-primary" id="net-save-btn">💾 Save</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(this.modal);
        this._setupEventListeners();
        this._validateIP();
    }

    _escapeHtml(text) {
        if (!text) return '';
        return String(text)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    _setupEventListeners() {
        // Protocol buttons
        this.modal.querySelectorAll('.protocol-btn').forEach(btn => {
            btn.onclick = () => {
                this.modal.querySelectorAll('.protocol-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                this.data.protocol = btn.dataset.protocol;
                
                // Show/hide HTTP-specific fields
                const isHttp = this.data.protocol === 'HTTP' || this.data.protocol === 'HTTPS';
                this.modal.querySelectorAll('.http-only').forEach(el => {
                    el.style.display = isHttp ? 'flex' : 'none';
                });
                
                // Update default port
                const portInput = this.modal.querySelector('#net-port');
                if (this.data.protocol === 'HTTP') portInput.value = 80;
                else if (this.data.protocol === 'HTTPS') portInput.value = 443;
                else if (this.data.protocol === 'WebSocket') portInput.value = 80;
            };
        });

        // Method buttons
        this.modal.querySelectorAll('.method-btn').forEach(btn => {
            btn.onclick = () => {
                this.modal.querySelectorAll('.method-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                this.data.method = btn.dataset.method;
            };
        });

        // IP inputs - auto-advance on full octet
        for (let i = 0; i < 4; i++) {
            const input = this.modal.querySelector(`#ip-${i}`);
            input.oninput = () => {
                if (input.value.length >= 3 && i < 3) {
                    this.modal.querySelector(`#ip-${i + 1}`).focus();
                }
                this._validateIP();
            };
            input.onchange = () => this._validateIP();
        }

        // Copy buttons
        this.modal.querySelector('#copy-path-btn').onclick = () => {
            navigator.clipboard.writeText(this.modal.querySelector('#net-path').value);
            this._notify('Path copied', 'success');
        };
        this.modal.querySelector('#copy-body-btn').onclick = () => {
            navigator.clipboard.writeText(this.modal.querySelector('#net-body').value);
            this._notify('Body copied', 'success');
        };

        // Action buttons
        this.modal.querySelector('#net-delete-btn').onclick = () => this._delete();
        this.modal.querySelector('#net-duplicate-btn').onclick = () => this._duplicate();
        this.modal.querySelector('#net-test-btn').onclick = () => this._test();
        this.modal.querySelector('#net-save-btn').onclick = () => this._save();
    }

    _validateIP() {
        const parts = [];
        for (let i = 0; i < 4; i++) {
            const val = parseInt(this.modal.querySelector(`#ip-${i}`).value) || 0;
            parts.push(val);
        }
        const valid = parts.every(p => p >= 0 && p <= 255) && parts.some(p => p > 0);
        const indicator = this.modal.querySelector('#ip-valid');
        indicator.textContent = valid ? '✓' : '';
        indicator.className = 'ip-valid ' + (valid ? 'valid' : 'invalid');
        return valid;
    }

    _getFormData() {
        const ipParts = [];
        for (let i = 0; i < 4; i++) {
            ipParts.push(this.modal.querySelector(`#ip-${i}`).value || '0');
        }
        
        return {
            name: this.modal.querySelector('#net-name').value,
            protocol: this.data.protocol,
            host: ipParts.join('.'),
            port: parseInt(this.modal.querySelector('#net-port').value) || 80,
            path: this.modal.querySelector('#net-path').value || '/',
            method: this.data.method,
            headers: this.data.headers || { "Content-Type": "application/json" },
            body: this.modal.querySelector('#net-body').value,
            timeout_ms: parseFloat(this.modal.querySelector('#net-delay').value || 5) * 1000,
            notes: this.modal.querySelector('#net-notes').value
        };
    }

    async _save() {
        const config = this._getFormData();
        
        if (!config.name) {
            this._notify('Please enter a name', 'error');
            return;
        }
        
        if (!this._validateIP()) {
            this._notify('Please enter a valid IP address', 'error');
            return;
        }

        try {
            // Save to the network_messages parameter
            await this.ws.send({
                type: 'set_param',
                comp: 'NetworkActions',
                param: 'network_messages',
                row: this.slotIndex,
                col: 0,
                value: JSON.stringify(config)
            });
            
            this._notify(`Saved network action: ${config.name}`, 'success');
            this.hide();
            
            if (this.onSave) {
                this.onSave(this.slotIndex, config);
            }
        } catch (e) {
            console.error('Failed to save network action:', e);
            this._notify('Failed to save', 'error');
        }
    }

    async _test() {
        const config = this._getFormData();
        
        if (!this._validateIP()) {
            this._notify('Please enter a valid IP address', 'error');
            return;
        }

        try {
            // Temporarily save config and trigger it
            await this.ws.send({
                type: 'set_param',
                comp: 'NetworkActions',
                param: 'network_messages',
                row: this.slotIndex,
                col: 0,
                value: JSON.stringify(config)
            });
            
            // Trigger the action
            await this.ws.send({
                type: 'set_param',
                comp: 'NetworkActions',
                param: 'trigger',
                row: 0,
                col: 0,
                value: this.slotIndex
            });
            
            this._notify(`Testing: ${config.name || 'Network Action'}`, 'info');
        } catch (e) {
            console.error('Failed to test network action:', e);
            this._notify('Test failed', 'error');
        }
    }

    async _delete() {
        if (!confirm('Delete this network action?')) return;
        
        try {
            await this.ws.send({
                type: 'set_param',
                comp: 'NetworkActions',
                param: 'network_messages',
                row: this.slotIndex,
                col: 0,
                value: ''
            });
            
            this._notify('Network action deleted', 'success');
            this.hide();
            
            if (this.onSave) {
                this.onSave(this.slotIndex, null);
            }
        } catch (e) {
            this._notify('Failed to delete', 'error');
        }
    }

    async _duplicate() {
        // Find next empty slot
        let nextSlot = -1;
        for (let i = this.slotIndex + 1; i < 100; i++) {
            try {
                const resp = await this.ws.send({
                    type: 'get_param',
                    comp: 'NetworkActions',
                    param: 'network_messages',
                    row: i,
                    col: 0
                });
                if (!resp.value) {
                    nextSlot = i;
                    break;
                }
            } catch (e) {
                nextSlot = i;
                break;
            }
        }
        
        if (nextSlot === -1) {
            this._notify('No empty slots available', 'error');
            return;
        }
        
        const config = this._getFormData();
        config.name = config.name + ' (copy)';
        
        try {
            await this.ws.send({
                type: 'set_param',
                comp: 'NetworkActions',
                param: 'network_messages',
                row: nextSlot,
                col: 0,
                value: JSON.stringify(config)
            });
            
            this._notify(`Duplicated to slot ${nextSlot}`, 'success');
            
            // Open the new slot
            this.slotIndex = nextSlot;
            this.data = config;
            this.hide();
            this.show(nextSlot, config, this.onSave);
        } catch (e) {
            this._notify('Failed to duplicate', 'error');
        }
    }
}


// Export for use
window.ConfigBuilder = {
    ParameterPicker,
    ExpressionBuilder,
    ActionBuilder,
    WatchSlotEditor,
    ActionManagerEditor,
    ExpressionMonitor,
    VariableManager,
    NetworkActionEditor
};
