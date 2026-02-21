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

const RGB_CHUNK_SIZE = 768; // Bytes per chunk - leaves headroom for base64 overhead

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
    const raw = buildRawBytes(frames, ledCount);
    const numChunks = Math.ceil(raw.length / RGB_CHUNK_SIZE);

    console.group(`[RGB] Upload: ${frames.length} frame(s), ${raw.length} bytes, ${numChunks} chunk(s)`);
    console.log('[RGB] LED count:', ledCount, '| Chunk size:', RGB_CHUNK_SIZE);

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
        const slice = raw.slice(i * RGB_CHUNK_SIZE, (i + 1) * RGB_CHUNK_SIZE);
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
        this._render();
    }

    _render() {
        this.container.innerHTML = `
        <div class="rgb-builder">

            <!-- Preset picker -->
            <div class="rgb-section">
                <h4>🎨 Presets</h4>
                <div class="rgb-preset-grid">
                    <button class="btn btn-secondary rgb-preset-btn" onclick="rgbBuilder._showPreset('solid')">⬛ Solid</button>
                    <button class="btn btn-secondary rgb-preset-btn" onclick="rgbBuilder._showPreset('rainbow')">🌈 Rainbow</button>
                    <button class="btn btn-secondary rgb-preset-btn" onclick="rgbBuilder._showPreset('breathing')">💫 Breathing</button>
                    <button class="btn btn-secondary rgb-preset-btn" onclick="rgbBuilder._showPreset('chase')">✨ Chase</button>
                    <button class="btn btn-danger rgb-preset-btn"    onclick="rgbBuilder._applyOff()">⏹️ Off</button>
                </div>
                <div id="rgb-preset-opts" class="rgb-preset-opts"></div>
            </div>

            <!-- Frame list -->
            <div class="rgb-section">
                <h4>🎞️ Frames <span id="rgb-frame-badge" class="rgb-badge">0</span></h4>
                <div id="rgb-frame-list" class="rgb-frame-list">
                    <div class="empty-hint">No frames yet — pick a preset above, or add one manually.</div>
                </div>
                <button class="btn btn-secondary" style="margin-top:8px" onclick="rgbBuilder._addBlankFrame()">
                    ➕ Add Blank Frame
                </button>
            </div>

            <!-- Upload & playback -->
            <div class="rgb-section">
                <h4>📡 Upload &amp; Playback</h4>
                <div class="rgb-controls-row">
                    <button class="btn btn-primary"   id="rgb-upload-btn"  onclick="rgbBuilder._upload()">⬆️ Upload</button>
                    <button class="btn btn-secondary" onclick="rgbBuilder._setPlaying(true)">▶️ Play</button>
                    <button class="btn btn-secondary" onclick="rgbBuilder._setPlaying(false)">⏹️ Stop</button>
                    <label class="rgb-loop-label">
                        <input type="checkbox" id="rgb-loop-chk" checked onchange="rgbBuilder._setLoop(this.checked)">
                        Loop
                    </label>
                </div>
                <div id="rgb-progress" class="rgb-progress" style="display:none">
                    <div class="rgb-progress-bg"><div class="rgb-progress-fill" id="rgb-progress-fill"></div></div>
                    <span id="rgb-progress-label">Uploading…</span>
                </div>
            </div>

        </div>`;
    }

    // ---------- Preset UI ----------

    _showPreset(type) {
        const opts = document.getElementById('rgb-preset-opts');
        const colorField = (id, label, def = '#ff0000') =>
            `<label>${label} <input type="color" id="${id}" value="${def}"></label>`;

        const forms = {
            solid: `
                ${colorField('rp-color', 'Color:')}
                <label>Duration (ms): <input type="number" id="rp-dur" value="1000" min="16" max="60000" style="width:90px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applySolid()">Apply</button>`,
            rainbow: `
                <label>Frames: <input type="number" id="rp-steps" value="60" min="4" max="360" style="width:70px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="50" min="16" max="2000" style="width:80px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyRainbow()">Apply</button>`,
            breathing: `
                ${colorField('rp-color', 'Color:')}
                <label>Frames: <input type="number" id="rp-steps" value="60" min="8" max="200" style="width:70px"></label>
                <label>Cycle (ms): <input type="number" id="rp-cycle" value="3000" min="200" max="20000" style="width:90px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyBreathing()">Apply</button>`,
            chase: `
                ${colorField('rp-color', 'Color:')}
                <label>Tail: <input type="number" id="rp-tail" value="5" min="1" max="30" style="width:60px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="50" min="16" max="1000" style="width:80px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyChase()">Apply</button>`,
        };
        opts.innerHTML = `<div class="rgb-preset-form">${forms[type] || ''}</div>`;
    }

    _val(id, fallback) {
        const el = document.getElementById(id);
        return el ? (el.type === 'number' ? (parseInt(el.value) || fallback) : el.value) : fallback;
    }

    _applyOff()       { this.frames = presetOff(this.ledCount); document.getElementById('rgb-preset-opts').innerHTML = ''; this._renderFrames(); }
    _applySolid()     { const [r,g,b] = hexToRgb(this._val('rp-color','#ff0000')); this.frames = presetSolid(this.ledCount, r, g, b, this._val('rp-dur', 1000)); this._renderFrames(); }
    _applyRainbow()   { this.frames = presetRainbow(this.ledCount, this._val('rp-steps', 60), this._val('rp-ms', 50)); this._renderFrames(); }
    _applyBreathing() { const [r,g,b] = hexToRgb(this._val('rp-color','#ff0000')); this.frames = presetBreathing(this.ledCount, r, g, b, this._val('rp-steps', 60), this._val('rp-cycle', 3000)); this._renderFrames(); }
    _applyChase()     { const [r,g,b] = hexToRgb(this._val('rp-color','#ff0000')); this.frames = presetChase(this.ledCount, r, g, b, this._val('rp-tail', 5), this._val('rp-ms', 50)); this._renderFrames(); }

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
        badge.textContent = this.frames.length;

        if (!this.frames.length) {
            list.innerHTML = '<div class="empty-hint">No frames yet — pick a preset above, or add one manually.</div>';
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

    // ---------- Upload & playback ----------

    async _upload() {
        if (!this.frames.length) { _rgbNotify('No frames to upload', 'error'); return; }
        if (!this.ws?.connected) { _rgbNotify('Not connected to device', 'error'); return; }

        const btn      = document.getElementById('rgb-upload-btn');
        const progress = document.getElementById('rgb-progress');
        const fill     = document.getElementById('rgb-progress-fill');
        const label    = document.getElementById('rgb-progress-label');

        btn.disabled = true;
        progress.style.display = 'block';

        try {
            // Stop playback before uploading so the LED task isn't reading
            // frames while we're overwriting them mid-transfer
            await this._setPlaying(false);

            await uploadAnimation(this.ws, this.compName, this.frames, this.ledCount, (msg, pct) => {
                fill.style.width = pct + '%';
                label.textContent = msg;
            });
            console.log(`[RGB] ✅ Upload success: ${this.frames.length} frame(s)`);
            _rgbNotify(`✅ Uploaded ${this.frames.length} frame(s)`, 'success');
        } catch (e) {
            console.error('[RGB] ✗ Upload failed:', e);
            _rgbNotify('Upload failed: ' + e.message, 'error');
        } finally {
            btn.disabled = false;
            setTimeout(() => { progress.style.display = 'none'; }, 3000);
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
