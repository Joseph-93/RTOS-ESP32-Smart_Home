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
        this.data = {
            expression: currentData.expression || '',
            variables: currentData.variables || {},  // Add variables tracking
            risingActions: currentData.risingActions || [],
            fallingActions: currentData.fallingActions || []
        };
        this._createModal();
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
                        <div class="actions-summary" id="rising-actions-summary">
                            ${this._summarizeActions(this.data.risingActions)}
                        </div>
                        <button class="btn btn-secondary" id="edit-rising-btn">✏️ Edit Rising Actions</button>
                    </div>
                    
                    <div class="watch-section">
                        <h3>📉 Falling Edge Actions <span class="edge-hint">(true → false)</span></h3>
                        <p class="helper-text">These actions run when the expression becomes false.</p>
                        <div class="actions-summary" id="falling-actions-summary">
                            ${this._summarizeActions(this.data.fallingActions)}
                        </div>
                        <button class="btn btn-secondary" id="edit-falling-btn">✏️ Edit Falling Actions</button>
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
        
        this.modal.querySelector('#slot-index-input').onchange = (e) => {
            this.slotIndex = parseInt(e.target.value) || 0;
            this.modal.querySelector('h2').textContent = `👁️ Watch Slot ${this.slotIndex} Configuration`;
        };
        this.modal.querySelector('#edit-expression-btn').onclick = () => this._editExpression();
        this.modal.querySelector('#edit-rising-btn').onclick = () => this._editRisingActions();
        this.modal.querySelector('#edit-falling-btn').onclick = () => this._editFallingActions();
        this.modal.querySelector('#clear-slot-btn').onclick = () => this._clearSlot();
        this.modal.querySelector('#save-watch-btn').onclick = () => this._save();
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
            if (a.type === 'SET') {
                return `<div class="action-summary-item">${i + 1}. SET ${a.comp}.${a.param}[${a.row}][${a.col}] = ${a.value}</div>`;
            } else if (a.type === 'NETWORK') {
                return `<div class="action-summary-item">${i + 1}. NETWORK message #${a.index}</div>`;
            }
            return '';
        }).join('');
    }

    async _editExpression() {
        // Pass existing variables to the expression builder
        const result = await this.expressionBuilder.show(this.data.expression, this.data.variables);
        if (result !== undefined && result !== null) {
            // Result now contains both expression and variables
            this.data.expression = result.expression;
            this.data.variables = result.variables;
            this.modal.querySelector('#current-expression').textContent = result.expression || '(not configured)';
            this.modal.querySelector('#current-variables').innerHTML = this._summarizeVariables();
        }
    }

    async _editRisingActions() {
        const result = await this.actionBuilder.show(this.data.risingActions);
        if (result) {
            this.data.risingActions = result;
            this.modal.querySelector('#rising-actions-summary').innerHTML = this._summarizeActions(result);
        }
    }

    async _editFallingActions() {
        const result = await this.actionBuilder.show(this.data.fallingActions);
        if (result) {
            this.data.fallingActions = result;
            this.modal.querySelector('#falling-actions-summary').innerHTML = this._summarizeActions(result);
        }
    }

    _clearSlot() {
        // Show inline confirmation instead of ugly confirm()
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
            this.data = { expression: '', variables: {}, risingActions: [], fallingActions: [] };
            this._save();
        };
    }

    _save() {
        this.hide();
        if (this.onSave) {
            this.onSave({
                slotIndex: this.slotIndex,
                expression: this.data.expression,
                variables: this.data.variables,  // Include variables in save data
                risingActions: this.data.risingActions,
                fallingActions: this.data.fallingActions
            });
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


// Export for use
window.ConfigBuilder = {
    ParameterPicker,
    ExpressionBuilder,
    ActionBuilder,
    WatchSlotEditor,
    ActionManagerEditor
};
