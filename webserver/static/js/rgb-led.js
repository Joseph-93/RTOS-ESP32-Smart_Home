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

// ============================================================================
// OKLCH - Perceptually Uniform Color Space
// Based on Björn Ottosson's OKLAB (2020)
// OKLCH is the cylindrical form: L=lightness, C=chroma, H=hue (0-360°)
// The hue channel is perceptually uniform - equal angle = equal perceived difference
// ============================================================================

/**
 * Convert linear sRGB (0-1) to OKLAB
 */
function linearRgbToOklab(r, g, b) {
    // RGB to LMS (cone responses)
    const l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    const m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    const s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    
    // Cube root (like CIELAB)
    const l_ = Math.cbrt(l);
    const m_ = Math.cbrt(m);
    const s_ = Math.cbrt(s);
    
    // LMS to OKLAB
    return [
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,  // L
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,  // a
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_   // b
    ];
}

/**
 * Convert OKLAB to linear sRGB (0-1)
 */
function oklabToLinearRgb(L, a, b) {
    // OKLAB to LMS
    const l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    const m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    const s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    
    // Cube (reverse of cbrt)
    const l = l_ * l_ * l_;
    const m = m_ * m_ * m_;
    const s = s_ * s_ * s_;
    
    // LMS to RGB
    return [
        +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    ];
}

/**
 * sRGB gamma correction: linear -> sRGB
 */
function linearToSrgb(x) {
    return x <= 0.0031308 ? 12.92 * x : 1.055 * Math.pow(x, 1/2.4) - 0.055;
}

/**
 * sRGB gamma correction: sRGB -> linear
 */
function srgbToLinear(x) {
    return x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4);
}

/**
 * Check if linear RGB values are within sRGB gamut
 */
function isInGamut(lr, lg, lb) {
    const epsilon = 0.0001;
    return lr >= -epsilon && lr <= 1 + epsilon &&
           lg >= -epsilon && lg <= 1 + epsilon &&
           lb >= -epsilon && lb <= 1 + epsilon;
}

/**
 * Find the maximum chroma that keeps the color in sRGB gamut
 * Uses binary search for efficiency
 */
function findMaxChroma(L, h, maxC = 0.4) {
    const hRad = h * Math.PI / 180;
    let lo = 0, hi = maxC;
    
    // Binary search for max valid chroma
    for (let i = 0; i < 20; i++) {
        const mid = (lo + hi) / 2;
        const a = mid * Math.cos(hRad);
        const b = mid * Math.sin(hRad);
        const [lr, lg, lb] = oklabToLinearRgb(L, a, b);
        
        if (isInGamut(lr, lg, lb)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * Find the lightness that allows maximum chroma for a given hue
 * @param {number} h - Hue in degrees
 * @returns {{L: number, C: number}} Optimal lightness and corresponding max chroma
 */
function findOptimalLightness(h) {
    let bestL = 0.5, bestC = 0;
    
    // Search for lightness that gives max chroma
    for (let L = 0.2; L <= 0.85; L += 0.05) {
        const maxC = findMaxChroma(L, h, 0.5);
        if (maxC > bestC) {
            bestC = maxC;
            bestL = L;
        }
    }
    return { L: bestL, C: bestC };
}

/**
 * Convert OKLCH to sRGB (0-255) with proper gamut mapping
 * @param {number} L - Lightness (0-1)
 * @param {number} C - Chroma (will be reduced if out of gamut)
 * @param {number} h - Hue (0-360 degrees)
 * @returns {number[]} [r, g, b] each 0-255
 */
function oklchToRgb(L, C, h) {
    // Find max chroma for this L and h
    const maxC = findMaxChroma(L, h, 0.4);
    const clampedC = Math.min(C, maxC);
    
    // OKLCH to OKLAB
    const hRad = h * Math.PI / 180;
    const a = clampedC * Math.cos(hRad);
    const b = clampedC * Math.sin(hRad);
    
    // OKLAB to linear RGB
    let [lr, lg, lb] = oklabToLinearRgb(L, a, b);
    
    // Soft clamp for floating point errors
    lr = Math.max(0, Math.min(1, lr));
    lg = Math.max(0, Math.min(1, lg));
    lb = Math.max(0, Math.min(1, lb));
    
    // Linear to sRGB with gamma
    const r = linearToSrgb(lr);
    const g = linearToSrgb(lg);
    const b2 = linearToSrgb(lb);
    
    return [
        Math.round(Math.max(0, Math.min(255, r * 255))),
        Math.round(Math.max(0, Math.min(255, g * 255))),
        Math.round(Math.max(0, Math.min(255, b2 * 255)))
    ];
}

/**
 * Convert OKLCH to sRGB with brightness scaling, using optimal lightness per hue
 * This maximizes saturation for each hue, then scales by brightness
 * @param {number} brightness - 0-100 percentage
 * @param {number} h - Hue (0-360 degrees)
 * @returns {number[]} [r, g, b] each 0-255
 */
function oklchToRgbBrightness(brightness, h) {
    // Find optimal L and C for this hue (max saturation)
    const { L: optimalL, C: optimalC } = findOptimalLightness(h);
    
    // Scale by brightness
    const scale = brightness / 100;
    const targetL = optimalL * scale;
    
    // Find max achievable chroma at the target lightness
    const maxC = findMaxChroma(targetL, h, 0.5);
    const useC = Math.min(optimalC * scale, maxC);
    
    // Convert to RGB
    const hRad = h * Math.PI / 180;
    const a = useC * Math.cos(hRad);
    const b = useC * Math.sin(hRad);
    
    let [lr, lg, lb] = oklabToLinearRgb(targetL, a, b);
    lr = Math.max(0, Math.min(1, lr));
    lg = Math.max(0, Math.min(1, lg));
    lb = Math.max(0, Math.min(1, lb));
    
    const r = linearToSrgb(lr);
    const g = linearToSrgb(lg);
    const b2 = linearToSrgb(lb);
    
    return [
        Math.round(Math.max(0, Math.min(255, r * 255))),
        Math.round(Math.max(0, Math.min(255, g * 255))),
        Math.round(Math.max(0, Math.min(255, b2 * 255)))
    ];
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

/**
 * Calculate perceived luminance of an RGB color (0-255 scale)
 * Based on ITU-R BT.709 coefficients for human eye sensitivity
 */
function rgbLuminance(r, g, b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/**
 * Normalize an RGB color to a target luminance
 * Scales all channels proportionally to achieve the target perceived brightness
 */
function normalizeToLuminance(r, g, b, targetLuminance) {
    const currentLum = rgbLuminance(r, g, b);
    if (currentLum <= 0) return [0, 0, 0];
    
    const scale = targetLuminance / currentLum;
    // Clamp to 255 and round
    return [
        Math.min(255, Math.round(r * scale)),
        Math.min(255, Math.round(g * scale)),
        Math.min(255, Math.round(b * scale))
    ];
}

/**
 * Rainbow preset generator
 * @param {number} ledCount - Number of LEDs
 * @param {number} steps - Number of animation frames
 * @param {number} stepMs - Duration per frame in ms
 * @param {number} brightness - Max brightness 0-100 (default 100)
 * @param {number} wavelength - LEDs per full rainbow cycle (360° of hue)
 *                              wavelength = ledCount → one full rainbow across strip
 *                              wavelength < ledCount → multiple rainbows
 *                              wavelength > ledCount → partial rainbow (slower gradient)
 * @param {boolean} normalize - If true, normalize all colors to equal perceived luminance
 *                              This makes red/green/blue appear as bright as yellow/cyan/etc.
 * @param {boolean} perceptual - If true, use OKLCH color space for perceptually uniform hue
 *                               Equal hue angle = equal perceived color difference
 */
function presetRainbow(ledCount, steps, stepMs, brightness = 100, wavelength = null, normalize = false, perceptual = false) {
    const wl = wavelength || ledCount;  // Default to one full rainbow
    
    // Target luminance for normalize mode
    const maxLuminance = rgbLuminance(0, 255, 0);  // ~182.4 (green is brightest)
    const targetLum = (brightness / 100) * maxLuminance;
    
    return Array.from({ length: steps }, (_, step) => {
        const offset = (step / steps) * 360;
        return {
            colors: Array.from({ length: ledCount }, (_, i) => {
                const hue = (offset + i * (360 / wl)) % 360;
                
                if (perceptual) {
                    // Use OKLCH with optimal lightness per hue
                    // This maximizes saturation for each color while keeping perceptual uniformity
                    return oklchToRgbBrightness(brightness, hue);
                } else {
                    // Use HSV (original behavior)
                    const [r, g, b] = hsvToRgb(hue, 1, 1);
                    
                    if (normalize) {
                        // Normalize to equal perceived luminance
                        return normalizeToLuminance(r, g, b, targetLum);
                    } else {
                        // Simple HSV brightness scaling
                        const scale = brightness / 100;
                        return [Math.round(r * scale), Math.round(g * scale), Math.round(b * scale)];
                    }
                }
            }),
            duration_ms: stepMs
        };
    });
}

/**
 * Breathing preset generator
 * @param {number} ledCount - Number of LEDs
 * @param {number} r,g,b - Base color RGB values (0-255)
 * @param {number} steps - Number of animation frames per cycle
 * @param {number} cycleMs - Total cycle duration in ms
 * @param {number} brightness - Max brightness 0-100 (default 100)
 * @param {number} curve - Curve exponent for non-linear brightness (default 1.0)
 *                         <1 = fast rise/slow fall (sqrt-like), >1 = slow rise/fast fall (squared-like)
 * @param {number} minBrightness - Min brightness 0-100 (default 0), baseline that animation never goes below
 */
function presetBreathing(ledCount, r, g, b, steps, cycleMs, brightness = 100, curve = 1.0, minBrightness = 0) {
    const stepMs = Math.round(cycleMs / steps);
    const maxBright = Math.max(0, Math.min(100, brightness)) / 100;
    const minBright = Math.max(0, Math.min(100, minBrightness)) / 100;
    return Array.from({ length: steps }, (_, step) => {
        // Base sinusoidal brightness (0 to 1)
        let bright = (Math.sin((step / steps) * 2 * Math.PI - Math.PI / 2) + 1) / 2;
        // Apply curve: bright^curve changes the shape
        bright = Math.pow(bright, curve);
        // Scale between min and max brightness
        bright = minBright + bright * (maxBright - minBright);
        const color = [Math.round(r * bright), Math.round(g * bright), Math.round(b * bright)];
        return { colors: Array(ledCount).fill(color), duration_ms: stepMs };
    });
}

/**
 * Chase preset generator
 * @param {number} ledCount - Number of LEDs
 * @param {number} r,g,b - Base color RGB values (0-255)
 * @param {number} tailLen - Number of LEDs in the trailing tail
 * @param {number} headLen - Number of LEDs in the leading head
 * @param {number} stepMs - Duration per frame in ms
 * @param {number} brightness - Max brightness 0-100 (default 100)
 * @param {number} curve - Curve exponent for tail/head fade (default 1.0)
 *                         <1 = gradual start, steep end (sqrt-like)
 *                         >1 = steep start, gradual end (squared-like)
 * @param {number} minBrightness - Min brightness 0-100 (default 0), baseline for unlit LEDs
 */
function presetChase(ledCount, r, g, b, tailLen, headLen, stepMs, brightness = 100, curve = 1.0, direction = 1, bounce = false, loop = -1, minBrightness = 0) {
    const maxBright = Math.max(0, Math.min(100, brightness)) / 100;
    const minBright = Math.max(0, Math.min(100, minBrightness)) / 100;
    
    // If loop=1 (play once), add lead-in (head enters) and lead-out (tail exits)
    // If loop=-1 or >1, just cycle through the strip positions for seamless looping
    const cyclic = (loop !== 1);
    
    // For loop=1: we need extra frames so head enters and tail exits fully
    // For cyclic: just use the base cycle length
    let numFrames;
    let frameOffset = 0;  // How many frames before root enters the strip
    
    if (bounce && ledCount > 1) {
        numFrames = 2 * (ledCount - 1);
        if (!cyclic) {
            // For bounce with loop=1, add lead-in and lead-out
            frameOffset = headLen;
            numFrames += headLen + tailLen;
        }
    } else {
        numFrames = ledCount;
        if (!cyclic) {
            // For wrap with loop=1, add lead-in and lead-out
            frameOffset = headLen;
            numFrames += headLen + tailLen;
        }
    }
    
    // Build position sequence - this is where the root "would be" at each logical step
    // For loop=1, root starts off-screen and ends off-screen
    const buildPos = (logicalIdx) => {
        // logicalIdx is the "internal" step, where 0 = root at first LED
        let pos;
        if (bounce && ledCount > 1) {
            const cycleLen = 2 * (ledCount - 1);
            const wrapped = ((logicalIdx % cycleLen) + cycleLen) % cycleLen;
            if (wrapped < ledCount) {
                pos = wrapped;
            } else {
                pos = 2 * (ledCount - 1) - wrapped;
            }
        } else {
            pos = ((logicalIdx % ledCount) + ledCount) % ledCount;
        }
        if (direction < 0) pos = ledCount - 1 - pos;
        return pos;
    };
    
    // Helper to get position at any frame index
    // For loop=1: frame 0 has root at position buildPos(-frameOffset), i.e., off-screen
    const getPos = (frameIdx, allowCyclic) => {
        const logicalIdx = frameIdx - frameOffset;
        
        if (!cyclic) {
            // For loop=1: positions outside the valid range are "off-screen"
            // Root enters from off-screen (negative logical idx) and exits off-screen
            const baseCycleLen = (bounce && ledCount > 1) ? 2 * (ledCount - 1) : ledCount;
            if (logicalIdx < 0 || logicalIdx >= baseCycleLen) {
                return -1;  // Off-screen, won't match any LED
            }
        }
        
        return buildPos(logicalIdx);
    };
    
    return Array.from({ length: numFrames }, (_, frameIdx) => ({
        colors: Array.from({ length: ledCount }, (_, ledIdx) => {
            let bright = 0;
            
            // Root (current position)
            if (getPos(frameIdx, cyclic) === ledIdx) {
                bright = Math.max(bright, 1);
            }
            
            // Tail: look back at history
            // Only use cyclic wrapping if loop != 1
            for (let k = 1; k <= tailLen; k++) {
                if (getPos(frameIdx - k, cyclic) === ledIdx) {
                    const linearFade = 1 - k / (tailLen + 1);
                    const tailBright = Math.pow(linearFade, curve);
                    bright = Math.max(bright, tailBright);
                }
            }
            
            // Head: look ahead at future positions
            // Only use cyclic wrapping if loop != 1
            for (let k = 1; k <= headLen; k++) {
                if (getPos(frameIdx + k, cyclic) === ledIdx) {
                    const linearFade = 1 - k / (headLen + 1);
                    const headBright = Math.pow(linearFade, curve);
                    bright = Math.max(bright, headBright);
                }
            }
            
            // Scale between min and max brightness
            bright = minBright + bright * (maxBright - minBright);
            
            if (bright <= 0) return [0, 0, 0];
            return [Math.round(r * bright), Math.round(g * bright), Math.round(b * bright)];
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
        this.activePreset = -1;       // What's ACTUALLY playing on LEDs (-1 = nothing)
        this.tier1Preset = -1;        // What's in tier 1 (override) - may not be playing if tier 0 has something
        this.tier2Preset = -1;        // What's in tier 2 (background) - may not be playing if higher tier has something
        this.presetCount = 0;
        this.memoryUsed = 0;
        this.memoryMax = 150 * 1024;
        
        // Server-side preset metadata (recipe info for editing)
        this.presetMetadata = {};     // {presetName: {effect_type, effect_params, ...}}
        
        // Editing state
        this.editingPresetName = null;
        this.editingPresetIndex = null;  // null = new preset, number = editing existing
        this.editingLoop = -1;           // Loop count: -1=infinite, 1=once, N=play N times
        this.editingEffectType = null;   // Effect type being edited
        this.editingEffectParams = {};   // Effect parameters for editing
        this.editorOpen = false;         // Is the editor panel visible?
        
        // Loading state
        this.isLoading = false;
        
        this._render();
        this._subscribeToParams();
        this._startPolling();  // Poll active_preset to catch when playback ends
    }
    
    // Show loading spinner on preset list
    _showPresetListLoading(text = 'Loading...') {
        const container = document.getElementById('rgb-preset-list');
        if (container) {
            container.innerHTML = `<div class="loading">${text}</div>`;
        }
    }
    
    // Show loading overlay on the whole builder
    _showBuilderLoading(text = 'Loading...') {
        this.isLoading = true;
        let overlay = document.getElementById('rgb-builder-loading');
        if (!overlay) {
            overlay = document.createElement('div');
            overlay.id = 'rgb-builder-loading';
            overlay.className = 'section-loading';
            this.container.style.position = 'relative';
            this.container.appendChild(overlay);
        }
        overlay.innerHTML = `<div class="spinner spinner-lg"></div><div class="loading-text">${text}</div>`;
        overlay.classList.remove('hidden');
    }
    
    // Hide builder loading overlay
    _hideBuilderLoading() {
        this.isLoading = false;
        const overlay = document.getElementById('rgb-builder-loading');
        if (overlay) {
            overlay.classList.add('hidden');
        }
    }
    
    // Poll active_preset and priority tiers every 500ms to catch changes
    _startPolling() {
        this._pollInterval = setInterval(async () => {
            if (!this.ws?.connected) return;
            try {
                let needsRender = false;
                
                // Check active_preset (what's ACTUALLY playing on LEDs)
                const resp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'active_preset', row: 0, col: 0 });
                const newActive = resp?.value ?? -1;
                if (newActive !== this.activePreset) {
                    console.log('[RGB] Poll: active_preset changed', this.activePreset, '->', newActive);
                    this.activePreset = newActive;
                    needsRender = true;
                }
                
                // Check tier 1 (override)
                const t1Resp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_priority', row: 1, col: 0 });
                const newT1 = t1Resp?.value ?? -1;
                if (newT1 !== this.tier1Preset) {
                    console.log('[RGB] Poll: tier1 changed', this.tier1Preset, '->', newT1);
                    this.tier1Preset = newT1;
                    needsRender = true;
                }
                
                // Check tier 2 (background)
                const t2Resp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_priority', row: 2, col: 0 });
                const newT2 = t2Resp?.value ?? -1;
                if (newT2 !== this.tier2Preset) {
                    console.log('[RGB] Poll: tier2 changed', this.tier2Preset, '->', newT2);
                    this.tier2Preset = newT2;
                    needsRender = true;
                }
                
                if (needsRender) this._renderPresetList();
            } catch (e) { /* ignore */ }
        }, 500);
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
        const params = ['preset_count', 'active_preset', 'anim_frame_count', 'anim_memory_used', 'anim_memory_max'];
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
            console.log('[RGB] Push update:', msg.param, '=', msg.value);
            this.onParamUpdate(msg.param, msg.value);
        }
    }
    
    async _refreshDeviceState() {
        if (!this.ws?.connected) return;
        
        this._showPresetListLoading('Loading presets...');
        
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
            
            // Get tier 1 (override) and tier 2 (background) from priority system
            const tier1Response = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_priority', row: 1, col: 0 });
            this.tier1Preset = tier1Response?.value ?? -1;
            const tier2Response = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_priority', row: 2, col: 0 });
            this.tier2Preset = tier2Response?.value ?? -1;
            
            this.memoryUsed = await getValue('anim_memory_used') ?? 0;
            this.memoryMax = await getValue('anim_memory_max') ?? 150 * 1024;
            
            console.log('[RGB] Device state:', { presetCount: this.presetCount, activePreset: this.activePreset, tier1: this.tier1Preset, tier2: this.tier2Preset, memoryUsed: this.memoryUsed, memoryMax: this.memoryMax });
            
            // Get list of actual preset IDs (may be non-sequential due to deletions)
            const presetIdsStr = await getValue('preset_ids') ?? '';
            const presetIds = presetIdsStr ? presetIdsStr.split(',').map(s => parseInt(s.trim())).filter(n => !isNaN(n)) : [];
            console.log('[RGB] Preset IDs:', presetIds);
            
            // Query each preset's metadata by its actual ID
            this.devicePresets = [];
            for (const id of presetIds) {
                // Set query index to the actual preset ID
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: id });
                await sleep(30);
                // Read results
                const name = await getValue('query_preset_name') ?? `Preset ${id}`;
                const frameCount = await getValue('query_preset_frame_count') ?? 0;
                const dataSize = await getValue('query_preset_data_size') ?? 0;
                const loop = await getValue('query_preset_loop') ?? -1;  // Default -1 (infinite) if param missing
                this.devicePresets.push({ id, name, frameCount, dataSize, loop });
            }
            
            // Load hub store metadata for all presets (effect recipes)
            await this._loadHubStoreMetadata();
            
            this._renderPresetList();
            this._renderMemoryBar();
        } catch (e) { 
            console.warn('[RGB] refresh state:', e); 
            this._renderPresetList(); // Render even on error to clear loading state
        }
    }
    
    // Called when param updates arrive via WebSocket
    onParamUpdate(param, value) {
        if (param === 'preset_count') {
            this.presetCount = value;
            this._refreshDeviceState(); // Re-fetch full metadata
        } else if (param === 'active_preset') {
            this.activePreset = value;
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
                        <label class="rgb-loop-label" title="-1 = infinite, 1 = play once, N = play N times">
                            Loop: <input type="number" id="rgb-loop-count" value="-1" min="-1" max="1000" style="width:60px" onchange="rgbBuilder.editingLoop = parseInt(this.value)">
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
        
        container.innerHTML = this.devicePresets.map((p) => {
            const id = p.id;  // Use actual preset ID, not array index
            const isPlaying = id === this.activePreset;   // Actually showing on LEDs right now
            const isInTier1 = id === this.tier1Preset;    // In tier 1 (override) - may or may not be playing
            const isInTier2 = id === this.tier2Preset;    // In tier 2 (background) - may or may not be playing
            const isEditing = id === this.editingPresetIndex;
            const name = p.name || `Preset ${id}`;
            const frameInfo = p.frameCount ? `${p.frameCount}f` : '';
            const loopIcon = p.loop === -1 ? '🔁' : `${p.loop}×`;
            
            // Build tier indicator text
            let tierIndicator = '';
            if (isInTier1) tierIndicator = ' ⚡';      // In tier 1
            if (isInTier2) tierIndicator = ' 🌙';      // In tier 2
            
            return `
            <div class="rgb-preset-row ${isPlaying ? 'active playing' : ''} ${isEditing ? 'editing' : ''}" data-preset-id="${id}">
                <div class="rgb-preset-main" onclick="rgbBuilder._editDevicePreset(${id})">
                    <span class="rgb-preset-name">${name}${isPlaying ? ' ▶' : ''}</span>
                    <span class="rgb-preset-meta">${frameInfo} ${loopIcon}${tierIndicator}</span>
                </div>
                <div class="rgb-preset-controls">
                    <button class="btn btn-sm ${isInTier2 ? 'btn-info' : 'btn-outline'}" 
                            onclick="event.stopPropagation(); rgbBuilder._setTier(${id}, 2)"
                            title="${isInTier2 ? 'Remove from tier 2' : 'Assign to tier 2 (background)'}">
                        🌙
                    </button>
                    <button class="btn btn-sm ${isInTier1 ? 'btn-warning' : 'btn-outline'}" 
                            onclick="event.stopPropagation(); rgbBuilder._setTier(${id}, 1)"
                            title="${isInTier1 ? 'Remove from tier 1' : 'Assign to tier 1 (override)'}">
                        ⚡
                    </button>
                    <button class="btn btn-sm btn-danger" 
                            onclick="event.stopPropagation(); rgbBuilder._deleteDevicePreset(${id})"
                            title="Delete">🗑️</button>
                </div>
            </div>`;
        }).join('');
    }
    
    async _setTier(presetId, tier) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        
        // Find preset by ID
        const preset = this.devicePresets.find(p => p.id === presetId);
        const name = preset?.name || `Preset ${presetId}`;
        const currentVal = tier === 1 ? this.tier1Preset : this.tier2Preset;
        const tierIcon = tier === 1 ? '⚡' : '🌙';
        
        try {
            if (presetId === currentVal) {
                // Already in this tier - remove it
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'preset_priority', row: tier, col: 0, value: -1 });
                if (tier === 1) this.tier1Preset = -1;
                else this.tier2Preset = -1;
                _rgbNotify(`${tierIcon} "${name}" removed from tier ${tier}`, 'success');
            } else {
                // Assign to this tier
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'preset_priority', row: tier, col: 0, value: presetId });
                if (tier === 1) this.tier1Preset = presetId;
                else this.tier2Preset = presetId;
                _rgbNotify(`${tierIcon} "${name}" assigned to tier ${tier}`, 'success');
            }
            this._renderPresetList();
        } catch (e) {
            _rgbNotify('Failed: ' + e.message, 'error');
        }
    }
    
    async _deleteDevicePreset(presetId) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        const preset = this.devicePresets.find(p => p.id === presetId);
        const name = preset?.name || `Preset #${presetId}`;
        if (!confirm(`Delete "${name}"? This cannot be undone.`)) return;
        try {
            // If we're editing this preset, close the editor
            if (this.editingPresetIndex === presetId) {
                this._closeEditor();
            }
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'delete_preset', row: 0, col: 0, value: presetId });
            
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
        this.editingLoop = -1;
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
        const loopInput = document.getElementById('rgb-loop-count');
        const saveBtn = document.getElementById('rgb-save-btn');
        
        if (panel) panel.style.display = 'block';
        if (titleEl) titleEl.textContent = `🎨 ${title}`;
        if (nameInput) nameInput.value = name;
        // Convert legacy boolean to integer: true -> -1, false -> 0
        const loopVal = (typeof loop === 'boolean') ? (loop ? -1 : 0) : (loop ?? -1);
        if (loopInput) loopInput.value = loopVal;
        this.editingLoop = loopVal;
        
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
        this.editingLoop = -1;
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
    async _editDevicePreset(presetId) {
        if (!this.ws?.connected) { _rgbNotify('Not connected', 'error'); return; }
        
        const preset = this.devicePresets.find(p => p.id === presetId);
        if (!preset) { _rgbNotify('Preset not found', 'error'); return; }
        
        const name = preset.name || `Preset ${presetId}`;
        const dataSize = preset.dataSize || 0;
        const frameCount = preset.frameCount || 0;
        // Handle legacy boolean or new integer loop: true->-1, false->0, number->number
        const loop = typeof preset.loop === 'boolean' ? (preset.loop ? -1 : 0) : (preset.loop ?? -1);
        
        // Generate unique download ID to detect stale downloads
        const downloadId = ++this._downloadId || (this._downloadId = 1);
        
        // CRITICAL: Clear frames FIRST to prevent stale data
        this.frames = [];
        this.editingPresetIndex = presetId;
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
        
        // Show loading overlay on editor panel
        this._showBuilderLoading(`Downloading "${name}"...`);
        
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
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: presetId });
            
            // CRITICAL: Reset chunk index to -1 BEFORE download loop
            // This ensures setting it to 0 actually triggers the onChange callback
            // If it's already 0 from a previous download, onChange won't fire!
            await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_download_chunk_index', row: 0, col: 0, value: -1 });
            await sleep(50);
            
            // Check if user switched to a different preset while we were waiting
            if (this._downloadId !== downloadId) {
                console.log('[RGB] Download cancelled - user switched presets');
                this._hideBuilderLoading();
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
                    this._hideBuilderLoading();
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
                this._hideBuilderLoading();
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
            this._hideBuilderLoading();
            _rgbNotify(`Loaded "${name}" (${this.frames.length} frames)`, 'success');
            
        } catch (e) {
            console.error('[RGB] Download failed:', e);
            this._hideBuilderLoading();
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
                <label>Frames: <input type="number" id="rp-steps" value="${savedParams.steps || 60}" min="4" style="width:70px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="${savedParams.step_ms || 50}" min="16" style="width:80px"></label>
                <label title="Maximum brightness percentage">Bright%: <input type="number" id="rp-brightness" value="${savedParams.brightness ?? 100}" min="1" max="100" style="width:60px"></label>
                <label title="LEDs per full rainbow cycle. Equal to LED count = 1 full rainbow. Smaller = more cycles, larger = slower gradient.">λ (LEDs/cycle): <input type="number" id="rp-wavelength" value="${savedParams.wavelength ?? this.ledCount}" min="1" step="1" style="width:70px"></label>
                <br>
                <label title="Use OKLCH color space instead of HSV. OKLCH has perceptually uniform hue - red/blue get equal visual time as green/purple. Also provides uniform brightness." style="cursor:pointer;">
                    <input type="checkbox" id="rp-perceptual" ${savedParams.perceptual ? 'checked' : ''}> OKLCH (perceptual)
                </label>
                <label title="(HSV only) Normalize all colors to equal perceived brightness. Without this, yellow/cyan appear brighter than red/blue." style="cursor:pointer;">
                    <input type="checkbox" id="rp-normalize" ${savedParams.normalize ? 'checked' : ''} ${savedParams.perceptual ? 'disabled' : ''}> Equal luminance
                </label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyRainbow()">Apply</button>`,
            breathing: `
                ${colorField('rp-color', 'Color:')}
                <label>Frames: <input type="number" id="rp-steps" value="${savedParams.steps || 60}" min="8" style="width:70px"></label>
                <label>Cycle (ms): <input type="number" id="rp-cycle" value="${savedParams.cycle_ms || 3000}" min="200" style="width:90px"></label>
                <label title="Minimum brightness percentage (baseline)">Min%: <input type="number" id="rp-min-brightness" value="${savedParams.minBrightness ?? 0}" min="0" max="100" style="width:60px"></label>
                <label title="Maximum brightness percentage">Max%: <input type="number" id="rp-brightness" value="${savedParams.brightness ?? 100}" min="1" max="100" style="width:60px"></label>
                <label title="Exponent (power). 1=linear, 2=x², 0.5=√x. Higher=slower rise then fast peak, lower=fast rise then slow peak.">Curve (^n): <input type="number" id="rp-curve" value="${savedParams.curve ?? 1}" min="0.1" step="0.1" style="width:60px"></label>
                <button class="btn btn-primary btn-sm" onclick="rgbBuilder._applyBreathing()">Apply</button>`,
            chase: `
                ${colorField('rp-color', 'Color:')}
                <label title="Leading fade-in length">Head: <input type="number" id="rp-head" value="${savedParams.head ?? 0}" min="0" style="width:60px"></label>
                <label title="Trailing fade-out length">Tail: <input type="number" id="rp-tail" value="${savedParams.tail ?? 5}" min="0" style="width:60px"></label>
                <label>ms/frame: <input type="number" id="rp-ms" value="${savedParams.step_ms || 50}" min="16" style="width:80px"></label>
                <label title="Minimum brightness percentage (baseline for unlit LEDs)">Min%: <input type="number" id="rp-min-brightness" value="${savedParams.minBrightness ?? 0}" min="0" max="100" style="width:60px"></label>
                <label title="Maximum brightness percentage">Max%: <input type="number" id="rp-brightness" value="${savedParams.brightness ?? 100}" min="1" max="100" style="width:60px"></label>
                <label title="Exponent (power) for tail/head fade. 1=linear, 2=concentrated near root, 0.5=spread out more.">Curve (^n): <input type="number" id="rp-curve" value="${savedParams.curve ?? 1}" min="0.1" step="0.1" style="width:60px"></label>
                <label title="Direction of chase movement">Dir:
                    <select id="rp-direction" style="padding:4px">
                        <option value="1" ${(savedParams.direction ?? 1) === 1 ? 'selected' : ''}>→ Forward</option>
                        <option value="-1" ${(savedParams.direction ?? 1) === -1 ? 'selected' : ''}>← Backward</option>
                    </select>
                </label>
                <label title="Bounce back at edges instead of wrapping around"><input type="checkbox" id="rp-bounce" ${savedParams.bounce ? 'checked' : ''}> Bounce</label>
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
        if (!el) return fallback;
        if (el.type === 'number') {
            const parsed = parseFloat(el.value);
            return isNaN(parsed) ? fallback : parsed;
        }
        return el.value;
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
        const brightness = this._val('rp-brightness', 100);
        const wavelength = this._val('rp-wavelength', this.ledCount);
        const normalize = document.getElementById('rp-normalize')?.checked || false;
        const perceptual = document.getElementById('rp-perceptual')?.checked || false;
        this.editingEffectType = 'rainbow';
        this.editingEffectParams = { steps, step_ms, brightness, wavelength, normalize, perceptual };
        this.frames = presetRainbow(this.ledCount, steps, step_ms, brightness, wavelength, normalize, perceptual);
        this._renderFrames();
    }
    
    _applyBreathing() {
        const color = this._val('rp-color', '#ff0000');
        const steps = this._val('rp-steps', 60);
        const cycle_ms = this._val('rp-cycle', 3000);
        const brightness = this._val('rp-brightness', 100);
        const minBrightness = this._val('rp-min-brightness', 0);
        const curve = this._val('rp-curve', 1);
        const [r,g,b] = hexToRgb(color);
        this.editingEffectType = 'breathing';
        this.editingEffectParams = { color, steps, cycle_ms, brightness, minBrightness, curve };
        this.frames = presetBreathing(this.ledCount, r, g, b, steps, cycle_ms, brightness, curve, minBrightness);
        this._renderFrames();
    }
    
    _applyChase() {
        const color = this._val('rp-color', '#ff0000');
        const head = this._val('rp-head', 0);
        const tail = this._val('rp-tail', 5);
        const step_ms = this._val('rp-ms', 50);
        const brightness = this._val('rp-brightness', 100);
        const minBrightness = this._val('rp-min-brightness', 0);
        const curve = this._val('rp-curve', 1);
        const direction = parseInt(document.getElementById('rp-direction')?.value || '1', 10);
        const bounce = document.getElementById('rp-bounce')?.checked || false;
        const [r,g,b] = hexToRgb(color);
        this.editingEffectType = 'chase';
        this.editingEffectParams = { color, head, tail, step_ms, brightness, minBrightness, curve, direction, bounce };
        this.frames = presetChase(this.ledCount, r, g, b, tail, head, step_ms, brightness, curve, direction, bounce, this.editingLoop, minBrightness);
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
        const loopInput = document.getElementById('rgb-loop-count');
        const isUpdate = this.editingPresetIndex !== null;

        if (btn) btn.disabled = true;
        if (progress) progress.style.display = 'block';

        try {
            // If updating an existing preset, delete it first
            // Note: Since preset IDs are stable, this won't affect other presets
            // But we should NOT stop playback - other presets may still be playing!
            if (isUpdate) {
                if (label) label.textContent = 'Deleting old preset...';
                if (fill) fill.style.width = '5%';
                console.log(`[RGB] Deleting old preset at index ${this.editingPresetIndex} before update`);
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'delete_preset', row: 0, col: 0, value: this.editingPresetIndex });
                await sleep(100);
            }
            
            // Get preset name and loop setting (integer: -1=infinite, 1=once, N=play N times)
            let presetName = nameInput?.value?.trim() || this.editingPresetName || '';
            const loopValue = loopInput ? parseInt(loopInput.value) : (this.editingLoop ?? -1);
            console.log(`[RGB] Save: nameInput=${nameInput}, value="${nameInput?.value}", trimmed="${nameInput?.value?.trim()}", presetName="${presetName}"`);
            
            // Set preset name - ALWAYS set it, even when updating
            if (presetName) {
                console.log(`[RGB] Setting anim_preset_name to: "${presetName}"`);
                await this.ws.send({ type: 'set_param', comp: this.compName, param: 'anim_preset_name', row: 0, col: 0, value: presetName });
                await sleep(30);
            } else {
                console.warn('[RGB] No preset name to set!');
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
            // ESP32 defaults to "Preset N" where N is the preset ID
            let actualPresetName = presetName;
            if (!actualPresetName) {
                try {
                    // Get preset IDs list and find the highest ID (most recently created)
                    const idsResp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'preset_ids', row: 0, col: 0 });
                    const idsStr = idsResp?.value || '';
                    const ids = idsStr.split(',').filter(s => s.trim()).map(s => parseInt(s.trim(), 10)).filter(n => !isNaN(n));
                    if (ids.length > 0) {
                        // The newest preset has the highest ID (IDs are monotonically increasing)
                        const newId = Math.max(...ids);
                        await this.ws.send({ type: 'set_param', comp: this.compName, param: 'query_preset_index', row: 0, col: 0, value: newId });
                        await sleep(30);
                        const nameResp = await this.ws.send({ type: 'get_param', comp: this.compName, param: 'query_preset_name', row: 0, col: 0 });
                        actualPresetName = nameResp?.value || '';
                        console.log(`[RGB] ESP32 assigned preset name: "${actualPresetName}" (ID ${newId})`);
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
