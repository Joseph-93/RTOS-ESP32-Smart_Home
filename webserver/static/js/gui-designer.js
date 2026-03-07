/**
 * LCD Color Designer
 *
 * Live 320×240 canvas preview of the ESP32 LCD + HSV color wheel picker.
 * Editable targets: background, per-button fill, per-button text color.
 *
 * ESP32 parameters driven:
 *   bg_color            (1×3 int)  [R, G, B]
 *   button_colors       (6×3 int)  [R, G, B] per button row
 *   button_text_colors  (6×3 int)  [R, G, B] per button row
 *   button_names        (6×1 str)  label text (read-only here)
 */

// ============================================================================
// Color helpers
// ============================================================================

function _gdHsvToRgb(h, s, v) {
    // h: 0-360, s: 0-1, v: 0-1  →  [r, g, b] each 0-255
    h = ((h % 360) + 360) % 360;
    const c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c;
    let r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return [Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255)];
}

function _gdRgbToHsv(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    let h = 0, s = max === 0 ? 0 : d / max, v = max;
    if (d !== 0) {
        if      (max === r) h = ((g - b) / d + 6) % 6 * 60;
        else if (max === g) h = ((b - r) / d + 2) * 60;
        else                h = ((r - g) / d + 4) * 60;
    }
    return [h, s, v];
}

function _gdRgbToHex(r, g, b) {
    return '#' + [r, g, b].map(v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0')).join('');
}

function _gdHexToRgb(hex) {
    hex = hex.replace(/^#/, '');
    if (hex.length === 3) hex = hex.split('').map(c => c + c).join('');
    if (hex.length !== 6) return [0, 0, 0];
    return [parseInt(hex.slice(0, 2), 16), parseInt(hex.slice(2, 4), 16), parseInt(hex.slice(4, 6), 16)];
}

// ============================================================================
// HSV Color Wheel (canvas-rendered disc)
// ============================================================================

class HsvColorWheel {
    /**
     * @param {HTMLCanvasElement} canvas   - Must be a square canvas
     * @param {function}          onChange - Called with (r, g, b) on every change
     */
    constructor(canvas, onChange) {
        this.canvas = canvas;
        this.ctx    = canvas.getContext('2d');
        this.size   = canvas.width;
        this.onChange = onChange;

        // State in HSV (easier to map from disc coords)
        this.H = 30;   // 0-360
        this.S = 0.85; // 0-1 (distance from center)
        this.V = 0.9;  // 0-1 (brightness, controlled by external slider)

        this._cached = null; // { V, imageData } — cached wheel disc
        this._dragging = false;

        this._bindEvents();
        this._render();
    }

    get _R()  { return this.size / 2 - 6; }
    get _cx() { return this.size / 2; }
    get _cy() { return this.size / 2; }

    setV(v) {
        this.V = Math.max(0, Math.min(1, v));
        this._cached = null; // invalidate — brightness affects every pixel
        this._render();
    }

    setRgb(r, g, b) {
        const [h, s, v] = _gdRgbToHsv(r, g, b);
        this.H = h; this.S = s; this.V = v;
        this._cached = null;
        this._render();
    }

    getRgb() { return _gdHsvToRgb(this.H, this.S, this.V); }

    _bindEvents() {
        const getCoords = (e) => {
            const rect = this.canvas.getBoundingClientRect();
            const sx = this.canvas.width  / rect.width;
            const sy = this.canvas.height / rect.height;
            const src = e.touches ? e.touches[0] : e;
            return { x: (src.clientX - rect.left) * sx, y: (src.clientY - rect.top) * sy };
        };

        const pick = (e) => {
            e.preventDefault();
            const { x, y } = getCoords(e);
            const dx = x - this._cx, dy = y - this._cy;
            const r  = Math.sqrt(dx * dx + dy * dy);
            if (!this._dragging && r > this._R + 10) return;

            this.H = ((Math.atan2(dy, dx) * 180 / Math.PI) + 360) % 360;
            this.S = Math.min(1, r / this._R);
            this._render();
            this.onChange(...this.getRgb());
        };

        this.canvas.addEventListener('mousedown',  (e) => { this._dragging = true;  pick(e); });
        window     .addEventListener('mousemove',  (e) => { if (this._dragging) pick(e); });
        window     .addEventListener('mouseup',    ()  => { this._dragging = false; });
        this.canvas.addEventListener('touchstart', (e) => { this._dragging = true;  pick(e); }, { passive: false });
        this.canvas.addEventListener('touchmove',  (e) => { if (this._dragging) pick(e); },     { passive: false });
        window     .addEventListener('touchend',   ()  => { this._dragging = false; });
    }

    _renderDisc() {
        if (this._cached && this._cached.V === this.V) return this._cached.imageData;

        const { size, ctx } = this;
        const cx = this._cx, cy = this._cy, R = this._R;
        const imageData = ctx.createImageData(size, size);
        const data = imageData.data;

        for (let y = 0; y < size; y++) {
            for (let x = 0; x < size; x++) {
                const dx = x - cx, dy = y - cy;
                const r  = Math.sqrt(dx * dx + dy * dy);
                const idx = (y * size + x) * 4;

                // Anti-alias the edge
                const alpha = Math.max(0, Math.min(1, R + 1 - r));
                if (alpha <= 0) { data[idx + 3] = 0; continue; }

                const h = ((Math.atan2(dy, dx) * 180 / Math.PI) + 360) % 360;
                const s = Math.min(1, r / R);
                const [rr, gg, bb] = _gdHsvToRgb(h, s, this.V);
                data[idx]     = rr;
                data[idx + 1] = gg;
                data[idx + 2] = bb;
                data[idx + 3] = Math.round(alpha * 255);
            }
        }

        this._cached = { V: this.V, imageData };
        return imageData;
    }

    _render() {
        const { ctx, size } = this;
        ctx.clearRect(0, 0, size, size);
        ctx.putImageData(this._renderDisc(), 0, 0);

        // Draw cursor
        const angle = this.H * Math.PI / 180;
        const dotX  = this._cx + this.S * this._R * Math.cos(angle);
        const dotY  = this._cy + this.S * this._R * Math.sin(angle);
        const [r, g, b] = this.getRgb();
        const bright = (r * 299 + g * 587 + b * 114) / 1000;

        ctx.save();
        // Outer ring
        ctx.beginPath();
        ctx.arc(dotX, dotY, 9, 0, 2 * Math.PI);
        ctx.strokeStyle = bright > 140 ? 'rgba(0,0,0,0.75)' : 'rgba(255,255,255,0.9)';
        ctx.lineWidth = 2.5;
        ctx.stroke();
        // Inner fill swatch
        ctx.beginPath();
        ctx.arc(dotX, dotY, 7, 0, 2 * Math.PI);
        ctx.fillStyle = `rgb(${r},${g},${b})`;
        ctx.fill();
        ctx.restore();
    }
}

// ============================================================================
// LCD Preview Canvas
// ============================================================================

class LcdPreview {
    /**
     * Mirrors the ESP32 createSimpleButtonGrid layout pixel-perfectly.
     * @param {HTMLCanvasElement} canvas - Display at CSS 320×240; may be 2x for retina
     */
    constructor(canvas) {
        this.canvas = canvas;
        this.ctx    = canvas.getContext('2d');
        this.scale  = canvas.width / 320; // CSS vs physical pixel scale

        // Defaults match ESP32 defaults
        this.bgColor           = [0, 0, 0];
        this.buttonColors      = [[0,100,200],[200,100,0],[0,150,100],[150,0,150],[200,0,0],[0,200,0]];
        this.buttonTextColors  = Array(6).fill(null).map(() => [255, 255, 255]);
        this.buttonNames       = ['Button 1','Button 2','Button 3','Button 4','Button 5','Button 6'];
        this.ipText            = 'Not connected';
        this.selectedTarget    = 'bg';

        this._render();
    }

    // Matches ESP32 button layout exactly (from createSimpleButtonGrid)
    _btnRect(i) {
        const W = 90, H = 60, hGap = 10, vGap = 15, startY = 50;
        return {
            x: 10 + (i % 3) * (W + hGap),
            y: startY + Math.floor(i / 3) * (H + vGap),
            w: W, h: H
        };
    }

    setSelected(t) { this.selectedTarget = t; this._render(); }

    /** Hit-test canvas-space coords (already scaled to canvas pixel coords) */
    hitTest(cx, cy) {
        const s = this.scale;
        const lx = cx / s, ly = cy / s;
        for (let i = 0; i < 6; i++) {
            const { x, y, w, h } = this._btnRect(i);
            if (lx >= x && lx <= x + w && ly >= y && ly <= y + h) return `btn${i}`;
        }
        return 'bg';
    }

    _render() {
        const ctx = this.ctx, s = this.scale;
        const W = 320 * s, H = 240 * s;

        // Background
        const [br, bg, bb] = this.bgColor;
        ctx.fillStyle = `rgb(${br},${bg},${bb})`;
        ctx.fillRect(0, 0, W, H);

        // Title
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'middle';
        ctx.font         = `bold ${Math.round(15 * s)}px -apple-system,BlinkMacSystemFont,sans-serif`;
        ctx.fillStyle    = 'white';
        ctx.fillText('Smart Home Controls', W / 2, 30 * s);

        // Buttons
        for (let i = 0; i < 6; i++) {
            const { x, y, w, h } = this._btnRect(i);
            const [fr, fg, fb]   = this.buttonColors[i]     || [80, 80, 80];
            const [tr, tg, tb]   = this.buttonTextColors[i] || [255, 255, 255];

            // Button fill
            ctx.fillStyle = `rgb(${fr},${fg},${fb})`;
            this._rrect(x * s, y * s, w * s, h * s, 5 * s);
            ctx.fill();

            // Selection highlight
            const sel = this.selectedTarget;
            if (sel === `btn${i}` || sel === `btntxt${i}`) {
                ctx.strokeStyle = sel.startsWith('btntxt') ? 'rgba(255,200,50,0.9)' : 'rgba(255,255,255,0.9)';
                ctx.lineWidth   = 2.5 * s;
                this._rrect((x - 1.5) * s, (y - 1.5) * s, (w + 3) * s, (h + 3) * s, 6 * s);
                ctx.stroke();
            }

            // Button text
            ctx.fillStyle    = `rgb(${tr},${tg},${tb})`;
            ctx.font         = `${Math.round(12 * s)}px -apple-system,BlinkMacSystemFont,sans-serif`;
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(this.buttonNames[i] || `Button ${i + 1}`, (x + w / 2) * s, (y + h / 2) * s);
        }

        // IP label
        ctx.font         = `${Math.round(10 * s)}px -apple-system,BlinkMacSystemFont,sans-serif`;
        ctx.fillStyle    = 'rgb(150,150,150)';
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'alphabetic';
        ctx.fillText(`IP: ${this.ipText}`, W / 2, 236 * s);

        // Background selection dashed border
        if (this.selectedTarget === 'bg') {
            ctx.strokeStyle = 'rgba(88,166,255,0.7)';
            ctx.lineWidth   = 2.5 * s;
            ctx.setLineDash([6 * s, 4 * s]);
            ctx.strokeRect(2 * s, 2 * s, W - 4 * s, H - 4 * s);
            ctx.setLineDash([]);
        }
    }

    _rrect(x, y, w, h, r) {
        const ctx = this.ctx;
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);         ctx.quadraticCurveTo(x + w, y,     x + w, y + r);
        ctx.lineTo(x + w, y + h - r);     ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
        ctx.lineTo(x + r, y + h);         ctx.quadraticCurveTo(x,     y + h, x,         y + h - r);
        ctx.lineTo(x, y + r);             ctx.quadraticCurveTo(x,     y,     x + r,     y);
        ctx.closePath();
    }
}

// ============================================================================
// GUI Designer Panel
// ============================================================================

class GuiDesigner {
    constructor(containerEl, ws, componentName) {
        this.container = containerEl;
        this.ws        = ws;
        this.comp      = componentName || 'GUI';

        // Color state — matches ESP32 defaults
        this.bgColor          = [0, 0, 0];
        this.buttonColors     = [[0,100,200],[200,100,0],[0,150,100],[150,0,150],[200,0,0],[0,200,0]];
        this.buttonTextColors = Array(6).fill(null).map(() => [255, 255, 255]);
        this.buttonNames      = ['Button 1','Button 2','Button 3','Button 4','Button 5','Button 6'];

        this.selectedTarget = 'bg';
        this._sendDebounce  = null;

        this._preview = null;
        this._wheel   = null;

        this._buildUI();
    }

    // -----------------------------------------------------------------------
    // Build
    // -----------------------------------------------------------------------

    _buildUI() {
        this.container.innerHTML = `
<div class="gd-panel">

  <!-- LEFT: LCD preview + target selector -->
  <div class="gd-left">
    <div class="gd-preview-label">LCD Preview  <span class="gd-hint">(click to select)</span></div>
    <div class="gd-lcd-wrap">
      <canvas id="gd-lcd" width="640" height="480" style="width:320px;height:240px;"></canvas>
    </div>

    <div class="gd-target-bar">
      <span class="gd-target-group-label">BG</span>
      <button class="gd-tb gd-tb-active" data-t="bg" title="Background">🖥</button>

      <span class="gd-target-group-label">Fill</span>
      ${[0,1,2,3,4,5].map(i => `
        <button class="gd-tb gd-tb-fill" data-t="btn${i}" title="Button ${i+1} fill">
          <span class="gd-tb-num">${i+1}</span>
        </button>`).join('')}

      <span class="gd-target-group-label">Text</span>
      ${[0,1,2,3,4,5].map(i => `
        <button class="gd-tb gd-tb-text" data-t="btntxt${i}" title="Button ${i+1} text color">
          <span>T${i+1}</span>
        </button>`).join('')}
    </div>
  </div>

  <!-- RIGHT: Color wheel + controls -->
  <div class="gd-right">
    <div class="gd-picker-title" id="gd-title">🖥️ Background</div>

    <div class="gd-wheel-wrap">
      <canvas id="gd-wheel" width="220" height="220"></canvas>
    </div>

    <div class="gd-brightness-row">
      <span class="gd-sl-icon" title="Brightness">☀️</span>
      <input type="range" id="gd-bright" class="gd-slider gd-bright-slider" min="0" max="100" value="90">
      <span class="gd-sl-val" id="gd-bright-val">90%</span>
    </div>

    <div class="gd-swatch-row">
      <div class="gd-swatch" id="gd-swatch"></div>
      <div class="gd-hex-box">
        <span class="gd-hash">#</span>
        <input type="text" id="gd-hex" class="gd-hex-input" maxlength="6" value="000000" spellcheck="false" autocomplete="off">
      </div>
      <div class="gd-rgb-label" id="gd-rgb">0, 0, 0</div>
    </div>

    <div class="gd-actions">
      <button class="btn-primary gd-send-all-btn" id="gd-send-all">✓ Apply All to Device</button>
      <button class="btn-secondary" id="gd-reload">↻ Load from Device</button>
    </div>

    <div class="gd-status" id="gd-status"></div>
  </div>

</div>`;

        // Spin up preview
        const lcdCanvas = this.container.querySelector('#gd-lcd');
        this._preview = new LcdPreview(lcdCanvas);
        this._syncPreview();
        this._preview.setSelected(this.selectedTarget);

        lcdCanvas.style.cursor = 'pointer';
        lcdCanvas.addEventListener('click', (e) => {
            const rect = lcdCanvas.getBoundingClientRect();
            const sx = lcdCanvas.width  / rect.width;
            const sy = lcdCanvas.height / rect.height;
            const target = this._preview.hitTest(
                (e.clientX - rect.left) * sx,
                (e.clientY - rect.top)  * sy
            );
            this._selectTarget(target);
        });

        // Spin up color wheel
        const wheelCanvas = this.container.querySelector('#gd-wheel');
        this._wheel = new HsvColorWheel(wheelCanvas, (r, g, b) => this._onPickerColor(r, g, b));

        // Brightness slider
        const brightSlider = this.container.querySelector('#gd-bright');
        brightSlider.addEventListener('input', () => {
            const v = parseInt(brightSlider.value) / 100;
            this.container.querySelector('#gd-bright-val').textContent = brightSlider.value + '%';
            this._wheel.setV(v);
            this._onPickerColor(...this._wheel.getRgb());
        });

        // Hex input
        const hexInput = this.container.querySelector('#gd-hex');
        hexInput.addEventListener('input', () => {
            const hex = hexInput.value.replace(/[^0-9a-fA-F]/g, '').slice(0, 6);
            hexInput.value = hex;
            if (hex.length === 6) {
                const [r, g, b] = _gdHexToRgb('#' + hex);
                this._wheel.setRgb(r, g, b);
                this._syncPickerUI(r, g, b);
                this._setTargetColor(this.selectedTarget, r, g, b);
                this._scheduleSend();
            }
        });

        // Target bar buttons
        this.container.querySelectorAll('.gd-tb').forEach(btn => {
            btn.addEventListener('click', () => this._selectTarget(btn.dataset.t));
        });

        // Action buttons
        this.container.querySelector('#gd-send-all').addEventListener('click', () => this._sendAll());
        this.container.querySelector('#gd-reload'  ).addEventListener('click', () => this.loadFromDevice());

        // Set initial picker state to bg color
        this._selectTarget('bg');
        this._updateTargetBarColors();
    }

    // -----------------------------------------------------------------------
    // Target management
    // -----------------------------------------------------------------------

    _selectTarget(target) {
        this.selectedTarget = target;

        // Update active state on target bar
        this.container.querySelectorAll('.gd-tb').forEach(btn => {
            btn.classList.toggle('gd-tb-active', btn.dataset.t === target);
        });

        // Update preview selection highlight
        this._preview.setSelected(target);

        // Update title
        const titleEl = this.container.querySelector('#gd-title');
        if (target === 'bg') {
            titleEl.textContent = '🖥️ Background';
        } else if (target.startsWith('btntxt')) {
            const i = parseInt(target.slice(6));
            titleEl.textContent = `🔤 Button ${i + 1} Text — "${this.buttonNames[i]}"`;
        } else {
            const i = parseInt(target.slice(3));
            titleEl.textContent = `🎨 Button ${i + 1} Fill — "${this.buttonNames[i]}"`;
        }

        // Sync color picker to current target color
        const [r, g, b] = this._getTargetColor(target);
        this._wheel.setRgb(r, g, b);
        this._syncPickerUI(r, g, b);
    }

    _getTargetColor(target) {
        if (target === 'bg') return [...this.bgColor];
        if (target.startsWith('btntxt')) return [...this.buttonTextColors[parseInt(target.slice(6))]];
        return [...this.buttonColors[parseInt(target.slice(3))]];
    }

    _setTargetColor(target, r, g, b) {
        if (target === 'bg') {
            this.bgColor = [r, g, b];
            this._preview.bgColor = [r, g, b];
        } else if (target.startsWith('btntxt')) {
            const i = parseInt(target.slice(6));
            this.buttonTextColors[i] = [r, g, b];
            this._preview.buttonTextColors[i] = [r, g, b];
        } else {
            const i = parseInt(target.slice(3));
            this.buttonColors[i] = [r, g, b];
            this._preview.buttonColors[i] = [r, g, b];
        }
        this._preview._render();
        this._updateTargetBarColors();
    }

    _updateTargetBarColors() {
        // Update fill buttons background
        for (let i = 0; i < 6; i++) {
            const fb = this.container.querySelector(`[data-t="btn${i}"]`);
            if (fb) fb.style.background = `rgb(${this.buttonColors[i].join(',')})`;

            // Update text color buttons (show text in its own color)
            const tb = this.container.querySelector(`[data-t="btntxt${i}"]`);
            if (tb) tb.style.color = `rgb(${this.buttonTextColors[i].join(',')})`;
        }
    }

    // -----------------------------------------------------------------------
    // Picker sync
    // -----------------------------------------------------------------------

    _onPickerColor(r, g, b) {
        this._syncPickerUI(r, g, b);
        this._setTargetColor(this.selectedTarget, r, g, b);
        this._scheduleSend();
    }

    _syncPickerUI(r, g, b) {
        const swatch = this.container.querySelector('#gd-swatch');
        if (swatch) swatch.style.background = `rgb(${r},${g},${b})`;

        const hexInput = this.container.querySelector('#gd-hex');
        if (hexInput && document.activeElement !== hexInput) {
            hexInput.value = _gdRgbToHex(r, g, b).slice(1).toUpperCase();
        }

        const rgbLabel = this.container.querySelector('#gd-rgb');
        if (rgbLabel) rgbLabel.textContent = `${r}, ${g}, ${b}`;

        // Keep brightness slider in sync with wheel's V
        if (this._wheel) {
            const brightSlider = this.container.querySelector('#gd-bright');
            const brightVal    = this.container.querySelector('#gd-bright-val');
            const pct = Math.round(this._wheel.V * 100);
            if (brightSlider) brightSlider.value = pct;
            if (brightVal)    brightVal.textContent = pct + '%';
        }
    }

    _syncPreview() {
        this._preview.bgColor          = [...this.bgColor];
        this._preview.buttonColors     = this.buttonColors.map(c => [...c]);
        this._preview.buttonTextColors = this.buttonTextColors.map(c => [...c]);
        this._preview.buttonNames      = [...this.buttonNames];
        this._preview._render();
    }

    // -----------------------------------------------------------------------
    // Send to device
    // -----------------------------------------------------------------------

    _scheduleSend() {
        clearTimeout(this._sendDebounce);
        this._sendDebounce = setTimeout(() => this._sendCurrent(), 180);
    }

    async _sendCurrent() {
        if (!this.ws?.connected) return;
        const t = this.selectedTarget;
        try {
            if (t === 'bg') {
                await this._sendParam('bg_color', 0, this.bgColor);
            } else if (t.startsWith('btntxt')) {
                const i = parseInt(t.slice(6));
                await this._sendParam('button_text_colors', i, this.buttonTextColors[i]);
            } else {
                const i = parseInt(t.slice(3));
                await this._sendParam('button_colors', i, this.buttonColors[i]);
            }
            this._setStatus('✓', 'ok');
        } catch (e) {
            this._setStatus('⚠ ' + e.message, 'err');
        }
    }

    async _sendAll() {
        if (!this.ws?.connected) { this._setStatus('Not connected', 'err'); return; }
        this._setStatus('Sending…', '');
        try {
            await this._sendParam('bg_color', 0, this.bgColor);
            for (let i = 0; i < 6; i++) {
                await this._sendParam('button_colors',     i, this.buttonColors[i]);
                await this._sendParam('button_text_colors', i, this.buttonTextColors[i]);
            }
            this._setStatus('✓ All colors applied!', 'ok');
        } catch (e) {
            this._setStatus('⚠ ' + e.message, 'err');
        }
    }

    async _sendParam(param, row, rgb) {
        for (let col = 0; col < 3; col++) {
            await this.ws.send({ type: 'set_param', comp: this.comp, param, row, col, value: rgb[col] });
        }
    }

    // -----------------------------------------------------------------------
    // Load from device
    // -----------------------------------------------------------------------

    async loadFromDevice() {
        if (!this.ws?.connected) { this._setStatus('Not connected', 'err'); return; }
        this._setStatus('Loading…', '');
        try {
            // Background
            const bgRGB = [];
            for (let c = 0; c < 3; c++) {
                const r = await this.ws.send({ type: 'get_param', comp: this.comp, param: 'bg_color', row: 0, col: c });
                bgRGB.push(r.value ?? 0);
            }
            this.bgColor = bgRGB;

            // Button fills + text colors + names
            for (let i = 0; i < 6; i++) {
                const fill = [], text = [];
                for (let c = 0; c < 3; c++) {
                    const rf = await this.ws.send({ type: 'get_param', comp: this.comp, param: 'button_colors',      row: i, col: c });
                    const rt = await this.ws.send({ type: 'get_param', comp: this.comp, param: 'button_text_colors', row: i, col: c });
                    fill.push(rf.value ?? 128);
                    text.push(rt.value ?? 255);
                }
                this.buttonColors[i]     = fill;
                this.buttonTextColors[i] = text;

                const rn = await this.ws.send({ type: 'get_param', comp: this.comp, param: 'button_names', row: i, col: 0 });
                if (rn.value) this.buttonNames[i] = rn.value;
            }

            this._syncPreview();
            this._updateTargetBarColors();
            this._selectTarget(this.selectedTarget); // refresh picker for current target
            this._setStatus('✓ Loaded from device', 'ok');
        } catch (e) {
            this._setStatus('⚠ Load failed: ' + e.message, 'err');
        }
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    _setStatus(msg, type) {
        const el = this.container.querySelector('#gd-status');
        if (!el) return;
        el.textContent = msg;
        el.className = 'gd-status' + (type ? ' gd-status-' + type : '');
        if (type === 'ok') setTimeout(() => { if (el.textContent === msg) { el.textContent = ''; el.className = 'gd-status'; } }, 3000);
    }
}

// ============================================================================
// Init (called from component.html)
// ============================================================================

let guiDesigner = null;

function initGuiDesigner(ws, componentName) {
    const container = document.getElementById('gui-designer-container');
    if (!container) return;
    guiDesigner = new GuiDesigner(container, ws, componentName);
    setTimeout(() => guiDesigner.loadFromDevice(), 600);
}
