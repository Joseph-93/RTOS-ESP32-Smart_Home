/**
 * RGB LED Animation Builder
 *
 * Provides the GUI panel for the RgbLed component page.
 * Handles preset generation, frame editing, chunked upload, and playback control.
 *
 * Frame format (per ESP32 spec):
 *   [R0,G0,B0, R1,G1,B1, ..., Rn,Gn,Bn, duration_ms_lo, duration_ms_hi]
 *   Frame size = (ledCount * 3) + 2 bytes
 *
 * Upload protocol:
 *   1. set_param anim_total_frames = N  (begins upload, allocates memory)
 *   2. For each chunk: set anim_chunk_index, then anim_chunk_data (base64)
 *   3. set_param anim_commit = true
 *   4. set_param playing = true
 */

// Chunk size is queried from ESP32 at runtime - no magic numbers!
let RGB_CHUNK_SIZE = 1024; // Default, will be overwritten by ESP32 value

// Simple toast notification - uses the page's showNotification if it exists,
// otherwise falls back to a self-contained inline toast.
function _rgbNotify(message, type = 'info') {
    if (typeof window.showNotification === 'function') {
        window.showNotification(message, type);
        return;
    }
    // Fallback: inject a small toast
    let toast = document.getElementById('_rgb-toast');
    if (!toast) {
        toast = document.createElement('div');
        toast.id = '_rgb-toast';
        toast.style.cssText = 'position:fixed;bottom:1.5rem;right:1.5rem;padding:0.6rem 1rem;border-radius:6px;font-size:0.9rem;z-index:9999;transition:opacity 0.4s;pointer-events:none;';
        document.body.appendChild(toast);
    }
    toast.textContent = message;
    toast.style.background = type === 'error' ? '#c0392b' : type === 'success' ? '#27ae60' : '#2c3e50';
    toast.style.color = '#fff';
    toast.style.opacity = '1';
    clearTimeout(toast._hide);
    toast._hide = setTimeout(() => { toast.style.opacity = '0'; }, 3000);
}

// ============================================================================
// Color / math helpers
// ============================================================================

function hsvToRgb(h, s, v) {
    // h: 0-360, s: 0-1, v: 0-1  →  [r, g, b] each 0-255
    const c = v * s;
    const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
    const m = v - c;
    let r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return [Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255)];
}

function hexToRgb(hex) {
    return [
        parseInt(hex.slice(1, 3), 16),
        parseInt(hex.slice(3, 5), 16),
        parseInt(hex.slice(5, 7), 16)
    ];
}

function rgbToHex(r, g, b) {
    return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}

// ============================================================================
// Frame data builders
// ============================================================================

function buildRawBytes(frames, ledCount) {
    const frameSize = (ledCount * 3) + 2;
    const buf = new Uint8Array(frames.length * frameSize);
    let offset = 0;
    for (const frame of frames) {
        for (let i = 0; i < ledCount; i++) {
            const [r, g, b] = frame.colors[i] || [0, 0, 0];
            buf[offset++] = r & 0xFF;
            buf[offset++] = g & 0xFF;
            buf[offset++] = b & 0xFF;
        }
        const dur = Math.max(0, Math.min(65535, frame.duration_ms | 0));
        buf[offset++] = dur & 0xFF;
        buf[offset++] = (dur >> 8) & 0xFF;
    }
    return buf;
}

function uint8ToBase64(bytes) {
    let binary = '';
    for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
    return btoa(binary);
}

// ============================================================================
// Preset generators
// ============================================================================

function presetOff(ledCount) {
    return [{ colors: Array(ledCount).fill([0, 0, 0]), duration_ms: 1000 }];
}

function presetSolid(ledCount, r, g, b, durationMs) {
    return [{ colors: Array(ledCount).fill([r, g, b]), duration_ms: durationMs }];
}

function presetRainbow(ledCount, steps, stepMs) {
    return Array.from({ length: steps }, (_, step) => {
        const offset = (step / steps) * 360;
        return {
            colors: Array.from({ length: ledCount }, (_, i) =>
                hsvToRgb((offset + (i / ledCount) * 360) % 360, 1, 1)
            ),
            duration_ms: stepMs
        };
    });
}

function presetBreathing(ledCount, r, g, b, steps, cycleMs) {
    const stepMs = Math.round(cycleMs / steps);
    return Array.from({ length: steps }, (_, step) => {
        const bright = (Math.sin((step / steps) * 2 * Math.PI - Math.PI / 2) + 1) / 2;
        const color = [Math.round(r * bright), Math.round(g * bright), Math.round(b * bright)];
        return { colors: Array(ledCount).fill(color), duration_ms: stepMs };
    });
}

function presetChase(ledCount, r, g, b, tailLen, stepMs) {
    return Array.from({ length: ledCount }, (_, pos) => ({
        colors: Array.from({ length: ledCount }, (_, i) => {
            const dist = (pos - i + ledCount) % ledCount;
            if (dist >= tailLen) return [0, 0, 0];
            const fade = 1 - dist / tailLen;
            return [Math.round(r * fade), Math.round(g * fade), Math.round(b * fade)];
        }),
        duration_ms: stepMs
    }));
}

// ============================================================================
// Upload logic
// ============================================================================

async function uploadAnimation(ws, compName, frames, ledCount, onProgress) {
    // CRITICAL: Query chunk size from ESP32 BEFORE uploading
    let chunkSize = RGB_CHUNK_SIZE;
    try {
        const resp = await ws.send({ type: 'get_param', comp: compName, param: 'chunk_size', row: 0, col: 0 });
        if (resp?.value && resp.value > 0) {
            chunkSize = resp.value;
            RGB_CHUNK_SIZE = chunkSize;  // Update global too
            console.log('[RGB] Confirmed chunk size from ESP32:', chunkSize);
        } else {
            console.warn('[RGB] Could not get chunk_size, using fallback:', chunkSize);
        }
    } catch (e) {
        console.warn('[RGB] Failed to query chunk_size:', e);
    }
    
    const raw = buildRawBytes(frames, ledCount);
    const numChunks = Math.ceil(raw.length / chunkSize);

    console.group(`[RGB] Upload: ${frames.length} frame(s), ${raw.length} bytes, ${numChunks} chunk(s)`);
    console.log('[RGB] LED count:', ledCount, '| Chunk size:', chunkSize);

    const send = async (label, msg) => {
        console.log(`[RGB] → ${label}`, msg.value !== undefined ? `value=${JSON.stringify(msg.value).substring(0, 80)}` : '');
        const resp = await ws.send(msg);
        console.log(`[RGB] ← ${label} response:`, resp);
        if (resp && resp.success === false) {
            console.error(`[RGB] ✗ ${label} FAILED:`, resp.error);
            throw new Error(`ESP32 rejected ${label}: ${resp.error || 'unknown error'}`);
        }
        console.log(`[RGB] ✓ ${label} OK`);
        return resp;
    };

    onProgress('Starting upload…', 0);
    console.log('[RGB] Setting total frames:', frames.length);
    await send('anim_total_frames',
        { type: 'set_param', comp: compName, param: 'anim_total_frames', row: 0, col: 0, value: frames.length });
    await sleep(150);

    for (let i = 0; i < numChunks; i++) {
        const slice = raw.slice(i * chunkSize, (i + 1) * chunkSize);
        const pct = Math.round((i / numChunks) * 90);
        onProgress(`Chunk ${i + 1} / ${numChunks}`, pct);
        console.log(`[RGB] Chunk ${i + 1}/${numChunks} — ${slice.length} bytes (${pct}%)`);
        await send(`chunk_index[${i}]`,
            { type: 'set_param', comp: compName, param: 'anim_chunk_index', row: 0, col: 0, value: i });
        await sleep(40);
        await send(`chunk_data[${i}]`,
            { type: 'set_param', comp: compName, param: 'anim_chunk_data', row: 0, col: 0, value: uint8ToBase64(slice) });
        await sleep(60);
    }

    onProgress('Committing…', 95);
    console.log('[RGB] Committing animation…');
    await send('anim_commit',
        { type: 'set_param', comp: compName, param: 'anim_commit', row: 0, col: 0, value: true });
    await sleep(150);
    onProgress('Done!', 100);
    console.log('[RGB] Upload complete ✓');
    console.groupEnd();
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ============================================================================
// Animation Builder class
// ============================================================================

class RgbLedAnimationBuilder {
    constructor(container, ws, compName, ledCount) {
        this.container = container;
        this.ws = ws;
        this.compName = compName;
        this.ledCount = ledCount;
        this.frames = [];
        
        // Device name for API calls (extracted from URL)
        this.deviceName = this._extractDeviceName();
        
        // Device preset state (read from ESP32)
        this.devicePresets = [];      // [{index, name, frameCount, dataSize, loop}]
        this.activePreset = -1;
        this.presetCount = 0;
        this.memoryUsed = 0;
        this.memoryMax = 150 * 1024;
        this.isPlaying = false;       // Actual playback state from ESP32
        
        // Server-side preset metadata (recipe info for editing)
        this.presetMetadata = {};     // {presetName: {effect_type, effect_params, ...}}
        
        // Editing state
        this.editingPresetName = null;
        this.editingPresetIndex = null;  // null = new preset, number = editing existing
        this.editingLoop = true;         // Loop setting for the preset being edited
        this.editingEffectType = null;   // Effect type being edited
        this.editingEffectParams = {};   // Effect parameters for editing
        this.editorOpen = false;         // Is the editor panel visible?
        
        this._render();
        this._subscribeToParams();
    }
    
    _extractDeviceName() {
        // Extract device name from URL: /device/esp32/RgbLed/ -> "esp32"
        const match = window.location.pathname.match(/\/device\/([^\/]+)\//);
        return match ? match[1] : 'unknown';
    }
    
    // ========== Hub Store API (ESP32-side metadata storage) ==========
    // The hub store is a key-value store on the ESP32 that survives reboots.
    // We use it to store preset "recipes" (effect type, params, etc.) so the
    // GUI can recall them later without the ESP32 caring about the data.
    
    async _loadHubStoreMetadata() {
        if (!this.ws?.connected) return;
        try {
            // Get the full dump from hub_store_dump param
            const resp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'hub_store_dump', row: 0, col: 0 });
            const json = resp?.value || '{}';
            console.log('[RGB] Hub store raw dump:', json);
            const data = JSON.parse(json);
            
            // Convert to presetMetadata format (keys are "preset_<name>")
            this.presetMetadata = {};
            for (const [key, value] of Object.entries(data)) {
                if (key.startsWith('preset_')) {
                    const presetName = key.substring(7); // Remove "preset_" prefix
                    try {
                        this.presetMetadata[presetName] = JSON.parse(value);
                        console.log(`[RGB] Loaded metadata for "${presetName}":`, this.presetMetadata[presetName]);
                    } catch (e) {
                        console.warn('[RGB] Failed to parse hub store entry:', key, e);
                    }
                }
            }
            console.log('[RGB] Loaded hub store metadata:', Object.keys(this.presetMetadata));
        } catch (e) {
            console.warn('[RGB] Failed to load hub store metadata:', e);
        }
    }
    
    async _saveHubStoreMetadata(presetName, metadata) {
        if (!this.ws?.connected) return;
        try {
            const key = `preset_${presetName}`;
            const value = JSON.stringify(metadata);
            
            // Set key, then set value
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'hub_store_key', row: 0, col: 0, value: key });
            await sleep(20);
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'hub_store_value', row: 0, col: 0, value: value });
            
            // Update local cache
            this.presetMetadata[presetName] = metadata;
            console.log('[RGB] Saved hub store metadata for:', presetName);
        } catch (e) {
            console.warn('[RGB] Failed to save hub store metadata:', e);
        }
    }
    
    async _deleteHubStoreMetadata(presetName) {
        if (!this.ws?.connected) return;
        try {
            const key = `preset_${presetName}`;
            
            // Set key, then delete (set value to empty string deletes)
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'hub_store_key', row: 0, col: 0, value: key });
            await sleep(20);
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'hub_store_value', row: 0, col: 0, value: '' });
            
            delete this.presetMetadata[presetName];
            console.log('[RGB] Deleted hub store metadata for:', presetName);
        } catch (e) {
            console.warn('[RGB] Failed to delete hub store metadata:', e);
        }
    }
    
    // ========== Subscriptions ==========
    
    // Subscribe to preset-related read-only params to keep UI in sync
    async _subscribeToParams() {
        if (!this.ws?.connected) return;
        const params = ['preset_count', 'active_preset', 'anim_frame_count', 'anim_memory_used', 'anim_memory_max', 'playing'];
        for (const p of params) {
            try {
                await this.ws.send({ type: 'subscribe', comp: this.compName, param: p });
            } catch (e) { console.warn(`[RGB] subscribe ${p}:`, e); }
        }
        // Also request current values
        this._refreshDeviceState();
        
        // Listen for push updates from ESP32
        window.addEventListener('esp32-push', (e) => this._handlePushEvent(e.detail));
    }
    
    _handlePushEvent(msg) {
        // Check if this is a param_update for our component
        if (msg.type === 'param_update' && msg.comp === this.compName) {
            this.onParamUpdate(msg.param, msg.value);
        }
    }
    
    async _refreshDeviceState() {
        if (!this.ws?.connected) return;
        try {
            // Fetch each param value individually
            const getValue = async (param) => {
                try {
                    const resp = await this.ws.send({ type: 'get_param', comp: this.compName, param, row: 0, col: 0 });
                    return resp?.value ?? null;
                } catch (e) { return null; }
            };
            
            // Query chunk size from ESP32 - no magic numbers!
            const espChunkSize = await getValue('chunk_size');
            if (espChunkSize && espChunkSize > 0) {
                RGB_CHUNK_SIZE = espChunkSize;
                console.log('[RGB] Chunk size from ESP32:', RGB_CHUNK_SIZE);
            } else {
                console.warn('[RGB] Could not get chunk_size from ESP32, using default:', RGB_CHUNK_SIZE);
            }
            
            this.presetCount = await getValue('preset_count') ?? 0;
            this.activePreset = await getValue('active_preset') ?? -1;
            this.memoryUsed = await getValue('anim_memory_used') ?? 0;
            this.memoryMax = await getValue('anim_memory_max') ?? 150 * 1024;
            this.isPlaying = await getValue('playing') ?? false;
            
            console.log('[RGB] Device state:', { presetCount: this.presetCount, activePreset: this.activePreset, isPlaying: this.isPlaying, memoryUsed: this.memoryUsed, memoryMax: this.memoryMax });
            
            // Query each preset's metadata (name, frame count, loop)
            this.devicePresets = [];
            for (let i = 0; i < this.presetCount; i++) {
                // Set query index
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: i });
                await sleep(30);
                // Read results
                const name = await getValue('query_preset_name') ?? `Preset ${i}`;
                const frameCount = await getValue('query_preset_frame_count') ?? 0;
                const dataSize = await getValue('query_preset_data_size') ?? 0;
                const loop = await getValue('query_preset_loop') ?? true;  // Default true if param missing
                this.devicePresets.push({ index: i, name, frameCount, dataSize, loop });
            }
            
            // Load hub store metadata for all presets (effect recipes)
            await this._loadHubStoreMetadata();
            
            this._renderPresetList();
            this._renderMemoryBar();
        } catch (e) { console.warn('[RGB] refresh state:', e); }
    }
    
    // Called when param updates arrive via WebSocket
    onParamUpdate(param, value) {
        if (param === 'preset_count') {
            this.presetCount = value;
            this._refreshDeviceState(); // Re-fetch full metadata
        } else if (param === 'active_preset') {
            this.activePreset = value;
            this._renderPresetList();
        } else if (param === 'playing') {
            this.isPlaying = value;
            this._renderPresetList();
        } else if (param === 'anim_memory_used') {
            this.memoryUsed = value;
            this._renderMemoryBar();
        } else if (param === 'anim_memory_max') {
            this.memoryMax = value;
            this._renderMemoryBar();
        }
    }

    _render() {
        this.container.innerHTML = `
        <div class="rgb-builder rgb-two-column">
            <!-- LEFT PANEL: Preset List -->
            <div class="rgb-panel rgb-panel-left">
                <div class="rgb-panel-header">
                    <h4>📦 Presets</h4>
                    <button class="btn btn-primary btn-sm" onclick="rgbBuilder._createNewPreset()">➕ New</button>
                </div>
                <div id="rgb-memory-bar" class="rgb-memory-bar"></div>
                <div id="rgb-preset-list" class="rgb-preset-list">
                    <div class="empty-hint">Loading…</div>
                </div>
            </div>
            
            <!-- RIGHT PANEL: Editor (hidden until preset selected) -->
            <div id="rgb-editor-panel" class="rgb-panel rgb-panel-right" style="display:none">
                <div class="rgb-panel-header">
                    <h4 id="rgb-editor-title">🎨 Editor</h4>
                    <button class="btn btn-secondary btn-sm" onclick="rgbBuilder._closeEditor()">✕ Close</button>
                </div>
                
                <!-- Preset metadata -->
                <div class="rgb-section">
                    <div class="rgb-editor-meta">
                        <label>Name: <input type="text" id="rgb-preset-name" placeholder="My Preset" class="rgb-name-input"></label>
                        <label class="rgb-loop-label">
                            <input type="checkbox" id="rgb-loop-chk" checked onchange="rgbBuilder.editingLoop = this.checked">
                            Loop
                        </label>
                    </div>
                </div>
                
                <!-- Effect generators -->
                <div class="rgb-section">
                    <h5>Effect Generators</h5>
                    <div id="rgb-effect-btns" class="rgb-preset-grid">
                        <button class="btn btn-secondary btn-sm rgb-effect-btn" data-effect="solid" onclick="rgbBuilder._showPreset('solid')">⬛ Solid</button>
                        <button class="btn btn-secondary btn-sm rgb-effect-btn" data-effect="rainbow" onclick="rgbBuilder._showPreset('rainbow')">🌈 Rainbow</button>
                        <button class="btn btn-secondary btn-sm rgb-effect-btn" data-effect="breathing" onclick="rgbBuilder._showPreset('breathing')">💫 Breathing</button>
                        <button class="btn btn-secondary btn-sm rgb-effect-btn" data-effect="chase" onclick="rgbBuilder._showPreset('chase')">✨ Chase</button>
                        <button class="btn btn-secondary btn-sm rgb-effect-btn" data-effect="off" onclick="rgbBuilder._applyOff()">⏹️ Off</button>
                    </div>
                    <div id="rgb-preset-opts" class="rgb-preset-opts"></div>
                </div>
                
                <!-- Frame list -->
                <div class="rgb-section">
                    <h5>Frames <span id="rgb-frame-badge" class="rgb-badge">0</span></h5>
                    <div id="rgb-frame-list" class="rgb-frame-list">
                        <div class="empty-hint">No frames — pick an effect above or add manually.</div>
                    </div>
                    <button class="btn btn-secondary btn-sm" style="margin-top:8px" onclick="rgbBuilder._addBlankFrame()">
                        ➕ Add Blank Frame
                    </button>
                </div>
                
                <!-- Save/Upload -->
                <div class="rgb-section rgb-editor-actions">
                    <button class="btn btn-primary" id="rgb-save-btn" onclick="rgbBuilder._save()">💾 Save Preset</button>
                    <button class="btn btn-secondary" onclick="rgbBuilder._closeEditor()">Cancel</button>
                </div>
                <div id="rgb-progress" class="rgb-progress" style="display:none">
                    <div class="rgb-progress-bg"><div class="rgb-progress-fill" id="rgb-progress-fill"></div></div>
                    <span id="rgb-progress-label">Uploading…</span>
                </div>
            </div>
        </div>`;
        
        this._renderMemoryBar();
        this._renderPresetList();
    }
    
    _renderMemoryBar() {
        const bar = document.getElementById('rgb-memory-bar');
        if (!bar) return;
        const pct = this.memoryMax > 0 ? Math.min(100, (this.memoryUsed / this.memoryMax) * 100) : 0;
        const usedKB = (this.memoryUsed / 1024).toFixed(1);
        const maxKB = (this.memoryMax / 1024).toFixed(0);
        const color = pct > 90 ? '#e74c3c' : pct > 70 ? '#f39c12' : '#27ae60';
        bar.innerHTML = `
            <div class="rgb-mem-label">Memory: ${usedKB} KB / ${maxKB} KB</div>
            <div class="rgb-mem-track"><div class="rgb-mem-fill" style="width:${pct}%;background:${color}"></div></div>
        `;
    }
    
    _renderPresetList() {
        const container = document.getElementById('rgb-preset-list');
        if (!container) return;
        
        if (this.presetCount === 0) {
            container.innerHTML = '<div class="empty-hint">No presets yet. Click "New" to create one!</div>';
            return;
        }
        
        container.innerHTML = this.devicePresets.map((p, i) => {
            const isActive = i === this.activePreset;
            const isPlaying = isActive && this.isPlaying;
            const isEditing = i === this.editingPresetIndex;
            const name = p.name || `Preset ${i}`;
            const frameInfo = p.frameCount ? `${p.frameCount}f` : '';
            const loopIcon = p.loop ? '🔁' : '1️⃣';
            return `
            <div class="rgb-preset-row ${isActive ? 'active' : ''} ${isPlaying ? 'playing' : ''} ${isEditing ? 'editing' : ''}">
                <div class="rgb-preset-main" onclick="rgbBuilder._editDevicePreset(${i})">
                    <span class="rgb-preset-name">${name}</span>
                    <span class="rgb-preset-meta">${frameInfo} ${loopIcon}</span>
                </div>
                <div class="rgb-preset-controls">
                    <button class="btn btn-sm ${isPlaying ? 'btn-primary' : 'btn-secondary'}" 
                            onclick="event.stopPropagation(); rgbBuilder._playPreset(${i})"
                            title="${isPlaying ? 'Stop' : 'Play'}">
                        ${isPlaying ? '⏹️' : '▶️'}
                    </button>
                    <button class="btn btn-sm btn-danger" 
                            onclick="event.stopPropagation(); rgbBuilder._deleteDevicePreset(${i})"
                            title="Delete">🗑️</button>
                </div>
            </div>`;
        }).join('');
    }
    
    async _playPreset(idx) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        try {
            const isCurrentlyPlaying = idx === this.activePreset && this.isPlaying;
            
            if (isCurrentlyPlaying) {
                // Stop playback
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'playing', row: 0, col: 0, value: false });
                this.isPlaying = false;
                _rgbNotify('⏹️ Stopped', 'success');
            } else {
                // Select preset and start playback
                if (idx !== this.activePreset) {
                    await this.ws.send({ type: 'set_param', comp: this.compName, param: 'active_preset', row: 0, col: 0, value: idx });
                    this.activePreset = idx;
                    await sleep(50);
                }
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'playing', row: 0, col: 0, value: true });
                this.isPlaying = true;
                const name = this.devicePresets[idx]?.name || `Preset ${idx}`;
                _rgbNotify(`▶️ Playing "${name}"`, 'success');
            }
            this._renderPresetList();
        } catch (e) {
            _rgbNotify('Failed: ' + e.message, 'error');
        }
    }
    
    async _deleteDevicePreset(idx) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        const preset = this.devicePresets[idx];
        const name = preset?.name || `Preset #${idx}`;
        if (!confirm(`Delete "${name}"? This cannot be undone.`)) return;
        try {
            // If we're editing this preset, close the editor
            if (this.editingPresetIndex === idx) {
                this._closeEditor();
            }
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'delete_preset', row: 0, col: 0, value: idx });
            
            // Also delete hub store metadata
            try {
                await this._deleteHubStoreMetadata(name);
            } catch (metaErr) {
                console.warn('[RGB] Failed to delete preset metadata:', metaErr);
            }
            
            _rgbNotify(`Deleted "${name}"`, 'success');
            // Refresh state
            setTimeout(() => this._refreshDeviceState(), 200);
        } catch (e) {
            _rgbNotify('Failed to delete preset: ' + e.message, 'error');
        }
    }
    
    // ========== Editor Methods ==========
    
    _createNewPreset() {
        // Clear any stale state FIRST
        this.frames = [];
        this.editingPresetIndex = null;
        this.editingPresetName = null;
        this.editingLoop = true;
        this.editingEffectType = null;
        this.editingEffectParams = {};
        this.editorOpen = true;
        
        // Clear any effect button highlights
        this._highlightEffectButton(null);
        
        this._openEditor('New Preset', '', true);
    }
    
    _openEditor(title, name, loop) {
        const panel = document.getElementById('rgb-editor-panel');
        const titleEl = document.getElementById('rgb-editor-title');
        const nameInput = document.getElementById('rgb-preset-name');
        const loopChk = document.getElementById('rgb-loop-chk');
        const saveBtn = document.getElementById('rgb-save-btn');
        
        if (panel) panel.style.display = 'block';
        if (titleEl) titleEl.textContent = `🎨 ${title}`;
        if (nameInput) nameInput.value = name;
        if (loopChk) loopChk.checked = loop;
        
        // Update save button text based on edit mode
        if (saveBtn) {
            saveBtn.textContent = this.editingPresetIndex !== null ? '💾 Update Preset' : '💾 Save New Preset';
        }
        
        this.editorOpen = true;
        this._renderFrames();
        
        // Re-render preset list to show editing highlight
        this._renderPresetList();
    }
    
    _closeEditor() {
        const panel = document.getElementById('rgb-editor-panel');
        if (panel) panel.style.display = 'none';
        
        // Clear editing state
        this.frames = [];
        this.editingPresetIndex = null;
        this.editingPresetName = null;
        this.editingLoop = true;
        this.editingEffectType = null;
        this.editingEffectParams = {};
        this.editorOpen = false;
        
        // Clear UI
        const nameInput = document.getElementById('rgb-preset-name');
        if (nameInput) nameInput.value = '';
        document.getElementById('rgb-preset-opts').innerHTML = '';
        
        // Clear effect button highlight
        this._highlightEffectButton(null);
        
        // Re-render preset list to remove editing highlight
        this._renderPresetList();
    }
    
    // Download preset data from device and load into editor
    async _editDevicePreset(idx) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        
        const preset = this.devicePresets[idx];
        if (!preset) { _rgbNotify('Preset not found', 'error'); return; }
        
        const name = preset.name || `Preset ${idx}`;
        const dataSize = preset.dataSize || 0;
        const frameCount = preset.frameCount || 0;
        const loop = preset.loop !== false;  // Default to true
        
        // Generate unique download ID to detect stale downloads
        const downloadId = ++this._downloadId || (this._downloadId = 1);
        
        // CRITICAL: Clear frames FIRST to prevent stale data
        this.frames = [];
        this.editingPresetIndex = idx;
        this.editingPresetName = name;
        this.editingLoop = loop;
        
        // Load metadata from hub store cache (populated by _loadHubStoreMetadata)
        const metadata = this.presetMetadata[name];
        console.log(`[RGB] Edit preset "${name}" - metadata from hub store:`, metadata);
        if (metadata) {
            this.editingEffectType = metadata.effect_type || 'custom';
            this.editingEffectParams = metadata.effect_params || {};
            console.log(`[RGB] Using effect "${this.editingEffectType}" with params:`, this.editingEffectParams);
        } else {
            this.editingEffectType = 'custom';
            this.editingEffectParams = {};
            console.log('[RGB] No metadata found, using custom effect');
        }
        
        // Open editor immediately with empty frames (will populate after download)
        this._openEditor(`Edit: ${name}`, name, loop);
        
        // If we have stored effect type, show that preset UI with params
        if (this.editingEffectType && this.editingEffectType !== 'custom') {
            console.log(`[RGB] Calling _showPreset("${this.editingEffectType}") with params:`, this.editingEffectParams);
            this._showPreset(this.editingEffectType);
        } else {
            console.log('[RGB] Skipping _showPreset - effect is custom or null');
        }
        
        if (dataSize === 0 || frameCount === 0) {
            _rgbNotify('Preset is empty — add frames in editor', 'info');
            return;
        }
        
        _rgbNotify(`Downloading "${name}"...`, 'info');
        
        try {
            // CRITICAL: Query chunk size from ESP32 BEFORE downloading
            let chunkSize = RGB_CHUNK_SIZE;
            try {
                const csResp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'chunk_size', row: 0, col: 0 });
                if (csResp?.value && csResp.value > 0) {
                    chunkSize = csResp.value;
                    RGB_CHUNK_SIZE = chunkSize;  // Update global too
                    console.log('[RGB] Confirmed chunk size from ESP32:', chunkSize);
                }
            } catch (e) {
                console.warn('[RGB] Failed to query chunk_size, using default:', chunkSize);
            }
            
            // Select preset for query
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: idx });
            
            // CRITICAL: Reset chunk index to -1 BEFORE download loop
            // This ensures setting it to 0 actually triggers the onChange callback
            // If it's already 0 from a previous download, onChange won't fire!
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_download_chunk_index', row: 0, col: 0, value: -1 });
            await sleep(50);
            
            // Check if user switched to a different preset while we were waiting
            if (this._downloadId !== downloadId) {
                console.log('[RGB] Download cancelled - user switched presets');
                return;
            }
            
            // Download chunks
            const numChunks = Math.ceil(dataSize / chunkSize);
            const rawData = new Uint8Array(dataSize);
            let offset = 0;
            
            for (let i = 0; i < numChunks; i++) {
                // Check for cancellation
                if (this._downloadId !== downloadId) {
                    console.log('[RGB] Download cancelled mid-stream');
                    return;
                }
                
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_download_chunk_index', row: 0, col: 0, value: i });
                await sleep(30);
                const resp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'query_download_chunk_data', row: 0, col: 0 });
                const b64 = resp?.value || '';
                if (!b64) break;
                
                // Decode base64
                const binary = atob(b64);
                for (let j = 0; j < binary.length && offset < dataSize; j++) {
                    rawData[offset++] = binary.charCodeAt(j);
                }
            }
            
            // Final check before populating frames
            if (this._downloadId !== downloadId) {
                console.log('[RGB] Download completed but user switched presets - discarding');
                return;
            }
            
            // Parse raw data into frames (this.frames was already cleared above)
            this.frames = [];  // Clear again just in case
            const frameSize = (this.ledCount * 3) + 2;
            let corruptFrames = 0;
            
            for (let f = 0; f < frameCount; f++) {
                const frameOffset = f * frameSize;
                if (frameOffset + frameSize > rawData.length) break;
                
                const colors = [];
                for (let led = 0; led < this.ledCount; led++) {
                    const r = rawData[frameOffset + led * 3 + 0];
                    const g = rawData[frameOffset + led * 3 + 1];
                    const b = rawData[frameOffset + led * 3 + 2];
                    colors.push([r, g, b]);
                }
                const durLo = rawData[frameOffset + this.ledCount * 3];
                const durHi = rawData[frameOffset + this.ledCount * 3 + 1];
                const duration_ms = durLo | (durHi << 8);
                
                // Validate duration - should be reasonable (1ms to 60s)
                if (duration_ms < 1 || duration_ms > 60000) {
                    corruptFrames++;
                    console.warn(`[RGB] Frame ${f} has suspicious duration: ${duration_ms}ms`);
                }
                
                this.frames.push({ colors, duration_ms });
            }
            
            if (corruptFrames > frameCount / 2) {
                console.error(`[RGB] ${corruptFrames}/${frameCount} frames have suspicious durations - preset data may be corrupt!`);
                _rgbNotify(`⚠️ Preset data appears corrupt (${corruptFrames} bad frames). Consider deleting and re-creating.`, 'error');
            }
            
            this._renderFrames();
            _rgbNotify(`Loaded "${name}" (${this.frames.length} frames)`, 'success');
            
        } catch (e) {
            console.error('[RGB] Download failed:', e);
            _rgbNotify('Download failed: ' + e.message, 'error');
        }
    }

    // ---------- Preset UI ----------

    _showPreset(type) {
        // Highlight the selected effect button
        this._highlightEffectButton(type);
        this.editingEffectType = type;
        
        const opts = document.getElementById('rgb-preset-opts');
        
        // Get saved params if editing an existing preset
        const savedParams = this.editingEffectParams || {};
        
        const colorField = (id, label, def = '#ff0000') => {
            const savedColor = savedParams.color || def;
            return `<label>${label} <input type="color" id="${id}" value="${savedColor}"></label>`;
        };

        const forms = {
            solid: `
                ${colorField('rp-color', 'Color:')}
                <label>Duration (ms): <input type="number" id="rp-dur" value="${savedParams.duration || 1000}" min="16" max="60000" style="width:90px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applySolid()">Apply</button>`,
            rainbow: `
                <label>Frames: <input type="number" id="rp-steps" value="${savedParams.steps || 60}" min="4" max="360" style="width:70px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="${savedParams.step_ms || 50}" min="16" max="2000" style="width:80px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyRainbow()">Apply</button>`,
            breathing: `
                ${colorField('rp-color', 'Color:')}
                <label>Frames: <input type="number" id="rp-steps" value="${savedParams.steps || 60}" min="8" max="200" style="width:70px"></label>
                <label>Cycle (ms): <input type="number" id="rp-cycle" value="${savedParams.cycle_ms || 3000}" min="200" max="20000" style="width:90px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyBreathing()">Apply</button>`,
            chase: `
                ${colorField('rp-color', 'Color:')}
                <label>Tail: <input type="number" id="rp-tail" value="${savedParams.tail || 5}" min="1" max="30" style="width:60px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="${savedParams.step_ms || 50}" min="16" max="1000" style="width:80px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyChase()">Apply</button>`,
        };
        opts.innerHTML = `<div class="rgb-preset-form">${forms[type] || ''}</div>`;
    }
    
    _highlightEffectButton(type) {
        // Remove active from all, add to selected
        document.querySelectorAll('#rgb-effect-btns .rgb-effect-btn').forEach(btn => {
            btn.classList.remove('active');
            if (btn.dataset.effect === type) {
                btn.classList.add('active');
            }
        });
    }

    _val(id, fallback) {
        const el = document.getElementById(id);
        return el ? (el.type === 'number' ? (parseInt(el.value) || fallback) : el.value) : fallback;
    }

    _applyOff() {
        this.editingEffectType = 'off';
        this.editingEffectParams = {};
        this.frames = presetOff(this.ledCount);
        this._highlightEffectButton('off');
        document.getElementById('rgb-preset-opts').innerHTML = '';
        this._renderFrames();
    }
    
    _applySolid() {
        const color = this._val('rp-color', '#ff0000');
        const duration = this._val('rp-dur', 1000);
        const [r,g,b] = hexToRgb(color);
        this.editingEffectType = 'solid';
        this.editingEffectParams = { color, duration };
        this.frames = presetSolid(this.ledCount, r, g, b, duration);
        this._renderFrames();
    }
    
    _applyRainbow() {
        const steps = this._val('rp-steps', 60);
        const step_ms = this._val('rp-ms', 50);
        this.editingEffectType = 'rainbow';
        this.editingEffectParams = { steps, step_ms };
        this.frames = presetRainbow(this.ledCount, steps, step_ms);
        this._renderFrames();
    }
    
    _applyBreathing() {
        const color = this._val('rp-color', '#ff0000');
        const steps = this._val('rp-steps', 60);
        const cycle_ms = this._val('rp-cycle', 3000);
        const [r,g,b] = hexToRgb(color);
        this.editingEffectType = 'breathing';
        this.editingEffectParams = { color, steps, cycle_ms };
        this.frames = presetBreathing(this.ledCount, r, g, b, steps, cycle_ms);
        this._renderFrames();
    }
    
    _applyChase() {
        const color = this._val('rp-color', '#ff0000');
        const tail = this._val('rp-tail', 5);
        const step_ms = this._val('rp-ms', 50);
        const [r,g,b] = hexToRgb(color);
        this.editingEffectType = 'chase';
        this.editingEffectParams = { color, tail, step_ms };
        this.frames = presetChase(this.ledCount, r, g, b, tail, step_ms);
        this._renderFrames();
    }

    // ---------- Frame list ----------

    _addBlankFrame() {
        this.frames.push({ colors: Array(this.ledCount).fill([255, 255, 255]), duration_ms: 500 });
        this._renderFrames();
    }

    _deleteFrame(idx) {
        this.frames.splice(idx, 1);
        this._renderFrames();
    }

    _editFrameColor(idx) {
        // Open a simple modal with a color picker + "apply to all LEDs" button
        const existing = this.frames[idx];
        const firstColor = existing.colors[0] || [255, 255, 255];
        const hex = rgbToHex(...firstColor);

        const modal = document.createElement('div');
        modal.className = 'config-modal-overlay';
        modal.innerHTML = `
            <div class="config-modal" style="max-width:360px">
                <div class="config-modal-header">
                    <h2>✏️ Edit Frame ${idx + 1} Color</h2>
                    <button class="close-btn" onclick="this.closest('.config-modal-overlay').remove()">✕</button>
                </div>
                <div class="config-modal-body" style="display:flex;flex-direction:column;gap:12px">
                    <label>Color (applies to all LEDs):
                        <input type="color" id="fc-picker" value="${hex}" style="width:100%;height:40px;margin-top:6px">
                    </label>
                    <label>Duration (ms):
                        <input type="number" id="fc-dur" value="${existing.duration_ms}" min="16" max="60000" style="width:100%;padding:6px;margin-top:6px">
                    </label>
                </div>
                <div class="config-modal-footer">
                    <button class="btn btn-secondary" onclick="this.closest('.config-modal-overlay').remove()">Cancel</button>
                    <button class="btn btn-primary" onclick="rgbBuilder._applyFrameEdit(${idx}); this.closest('.config-modal-overlay').remove()">Apply</button>
                </div>
            </div>`;
        document.getElementById('config-modal-container').appendChild(modal);
    }

    _applyFrameEdit(idx) {
        const hex = document.getElementById('fc-picker')?.value || '#ffffff';
        const dur = parseInt(document.getElementById('fc-dur')?.value) || 500;
        const [r, g, b] = hexToRgb(hex);
        this.frames[idx] = { colors: Array(this.ledCount).fill([r, g, b]), duration_ms: dur };
        this._renderFrames();
    }

    _renderFrames() {
        const list   = document.getElementById('rgb-frame-list');
        const badge  = document.getElementById('rgb-frame-badge');
        if (badge) badge.textContent = this.frames.length;

        if (!list) return;
        
        if (!this.frames.length) {
            list.innerHTML = '<div class="empty-hint">No frames — pick an effect above or add manually.</div>';
            return;
        }

        // Show max 200 rows for performance; show a note if more
        const visible = this.frames.slice(0, 200);
        const overflow = this.frames.length - visible.length;

        list.innerHTML = visible.map((frame, idx) => {
            // Build LED preview — sample up to 40 swatches evenly
            const n = Math.min(40, this.ledCount);
            const swatches = Array.from({ length: n }, (_, si) => {
                const [r, g, b] = frame.colors[Math.floor(si * this.ledCount / n)] || [0, 0, 0];
                return `<span class="rgb-swatch" style="background:rgb(${r},${g},${b})"></span>`;
            }).join('');

            return `
            <div class="rgb-frame-row">
                <span class="rgb-frame-num">${idx + 1}</span>
                <div class="rgb-frame-preview">${swatches}</div>
                <input class="rgb-dur-input" type="number" value="${frame.duration_ms}" min="16" max="60000"
                    title="Duration ms"
                    onchange="rgbBuilder.frames[${idx}].duration_ms = Math.max(16, parseInt(this.value) || 50)">
                <span style="color:var(--text-secondary);font-size:.8em">ms</span>
                <button class="btn-icon" onclick="rgbBuilder._editFrameColor(${idx})" title="Edit color">✏️</button>
                <button class="btn-icon" onclick="rgbBuilder._deleteFrame(${idx})" title="Delete">🗑️</button>
            </div>`;
        }).join('') + (overflow ? `<div class="empty-hint">… and ${overflow} more frames (not shown)</div>` : '');
    }

    // ---------- Save (Upload) ----------

    async _save() {
        if (!this.frames.length) { _rgbNotify('No frames to save', 'error'); return; }
        if (!this.ws?.connected) { _rgbNotify('Not connected to device', 'error'); return; }

        const btn      = document.getElementById('rgb-save-btn');
        const progress = document.getElementById('rgb-progress');
        const fill     = document.getElementById('rgb-progress-fill');
        const label    = document.getElementById('rgb-progress-label');
        const nameInput = document.getElementById('rgb-preset-name');
        const loopChk   = document.getElementById('rgb-loop-chk');
        const isUpdate = this.editingPresetIndex !== null;

        if (btn) btn.disabled = true;
        if (progress) progress.style.display = 'block';

        try {
            // Stop playback before uploading
            await this._setPlayingSilent(false);
            
            // If updating an existing preset, delete it first
            if (isUpdate) {
                if (label) label.textContent = 'Deleting old preset...';
                if (fill) fill.style.width = '5%';
                console.log(`[RGB] Deleting old preset at index ${this.editingPresetIndex} before update`);
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'delete_preset', row: 0, col: 0, value: this.editingPresetIndex });
                await sleep(100);
            }
            
            // Get preset name and loop setting
            let presetName = nameInput?.value?.trim() || this.editingPresetName || '';
            const loopValue = loopChk?.checked ?? this.editingLoop ?? true;
            
            // Set preset name
            if (presetName) {
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'anim_preset_name', row: 0, col: 0, value: presetName });
                await sleep(30);
            }
            
            // Set loop setting (ignore errors if firmware doesn't support it yet)
            try {
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'anim_upload_loop', row: 0, col: 0, value: loopValue });
                await sleep(30);
            } catch (e) {
                console.warn('[RGB] anim_upload_loop param not supported (firmware needs update)');
            }

            await uploadAnimation(this.ws, this.compName, this.frames, this.ledCount, (msg, pct) => {
                if (fill) fill.style.width = pct + '%';
                if (label) label.textContent = msg;
            });
            
            // If we didn't provide a name, query the ESP32 for the name it assigned
            // ESP32 defaults to "Preset N" where N is the preset index
            let actualPresetName = presetName;
            if (!actualPresetName) {
                try {
                    // Get the new preset count (just created preset is at index count-1)
                    const countResp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_count', row: 0, col: 0 });
                    const newCount = countResp?.value || 0;
                    if (newCount > 0) {
                        // Query the name of the last preset (newly created)
                        const newIdx = newCount - 1;
                        await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: newIdx });
                        await sleep(30);
                        const nameResp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'query_preset_name', row: 0, col: 0 });
                        actualPresetName = nameResp?.value || '';
                        console.log(`[RGB] ESP32 assigned preset name: "${actualPresetName}"`);
                    }
                } catch (e) {
                    console.warn('[RGB] Failed to query assigned preset name:', e);
                }
            }
            
            const displayName = actualPresetName || 'Preset';
            const action = isUpdate ? 'Updated' : 'Saved';
            console.log(`[RGB] ✅ ${action}: "${displayName}" (${this.frames.length} frame(s))`);
            _rgbNotify(`✅ ${action} "${displayName}" (${this.frames.length} frames)`, 'success');
            
            // Save metadata to hub store (effect type, params, loop, frame count)
            // Use the ACTUAL name (either provided or ESP32-assigned)
            if (actualPresetName) {
                try {
                    await this._saveHubStoreMetadata(actualPresetName, {
                        effect_type: this.editingEffectType || 'custom',
                        effect_params: this.editingEffectParams || {},
                        loop: loopValue,
                        frame_count: this.frames.length
                    });
                } catch (metaErr) {
                    console.warn('[RGB] Failed to save preset metadata:', metaErr);
                    // Don't fail the save, just warn
                }
            } else {
                console.warn('[RGB] No preset name available, skipping metadata save');
            }
            
            // Close editor and refresh
            this._closeEditor();
            setTimeout(() => this._refreshDeviceState(), 300);
        } catch (e) {
            console.error('[RGB] ✗ Save failed:', e);
            _rgbNotify('Save failed: ' + e.message, 'error');
        } finally {
            if (btn) btn.disabled = false;
            setTimeout(() => { if (progress) progress.style.display = 'none'; }, 3000);
        }
    }

    async _setPlayingSilent(val) {
        // Set playing without showing notification
        if (!this.ws?.connected) return;
        try {
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'playing', row: 0, col: 0, value: val });
        } catch (e) {
            console.warn('[RGB] setPlayingSilent failed:', e);
        }
    }

    async _setPlaying(val) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        console.log(`[RGB] setPlaying(${val})`);
        try {
            const resp = await this.ws.send({ type: 'set_param', comp: this.compName, param: 'playing', row: 0, col: 0, value: val });
            console.log('[RGB] setPlaying response:', resp);
            if (resp && resp.success === false) throw new Error(resp.error || 'ESP32 rejected');
            _rgbNotify(val ? '▶️ Playing' : '⏹️ Stopped', 'success');
        } catch (e) {
            console.error('[RGB] setPlaying failed:', e);
            _rgbNotify(`Failed to ${val ? 'start' : 'stop'} playback: ${e.message}`, 'error');
        }
    }

    async _setLoop(val) {
        if (!this.ws?.connected) return;
        console.log(`[RGB] setLoop(${val})`);
        try {
            const resp = await this.ws.send({ type: 'set_param', comp: this.compName, param: 'loop', row: 0, col: 0, value: val });
            console.log('[RGB] setLoop response:', resp);
            if (resp && resp.success === false) throw new Error(resp.error || 'ESP32 rejected');
        } catch (e) {
            console.error('[RGB] setLoop failed:', e);
            _rgbNotify(`Failed to set loop: ${e.message}`, 'error');
        }
    }
}

// ============================================================================
// Entry point called from component.html
// ============================================================================

let rgbBuilder = null;

async function initRgbLedBuilder(ws, compName) {
    const ledCount = 30;  // Hardcoded — matches RGB_LED_COUNT in firmware

    const container = document.getElementById('rgb-led-builder-container');
    if (!container) return;

    document.getElementById('rgb-led-count-display').textContent = ledCount;
    rgbBuilder = new RgbLedAnimationBuilder(container, ws, compName, ledCount);
}
