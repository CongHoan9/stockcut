// ============================================================================
// js/render-worker.js
// Dedicated Web Worker Render Engine & Lock-Free Multi-Threading Presenter
// Tinh hoa Stockfish: Shared-Nothing Worker, OffscreenCanvas, Zero-Lock Hot Path
// ============================================================================
'use strict';

let gl = null;
let offscreenCanvas = null;
let wasmModule = null;
let wasmReady = false;
let isPlaying = false;
let lastPlayTimestamp = 0;
let playbackTimeMs = 0;
let frameTick = 0;
let activeVideoBitmaps = new Map();

// Local cache for textures inside Web Worker WebGL context
const workerTexCache = new Map(); // resId -> WebGLTexture / texId
const slotTexMap = new Uint32Array(64);
let consumer = null;
let loopRunning = false;

function postWorkerMessage(type, payload = {}) {
    self.postMessage({ type, ...payload });
}

// 1. Initialize WebAssembly Module in Worker
async function initWasmInWorker(wasmBinary = null) {
    try {
        if (typeof importScripts === 'function') {
            try {
                importScripts('stockcut_runtime.js');
            } catch (e) {
                try {
                    importScripts('../stockcut_runtime.js');
                } catch (e2) {
                    console.warn('[RenderWorker] importScripts stockcut_runtime.js fallback:', e2);
                }
            }
        }
        if (typeof self.createStockcutModule !== 'function') {
            throw new Error('[RenderWorker] createStockcutModule not found.');
        }

        const config = {
            noInitialRun: true,
            canvas: offscreenCanvas,
            print: (text) => console.log('[RenderWorker WASM]', text),
            printErr: (text) => console.warn('[RenderWorker WASM ERR]', text)
        };
        if (wasmBinary) {
            config.wasmBinary = wasmBinary;
        }

        wasmModule = await self.createStockcutModule(config);
        wasmReady = true;
        preloadWorkerStaticEffectTextures();
        postWorkerMessage('READY', { success: true });
    } catch (err) {
        console.error('[RenderWorker] initWasm error:', err);
        postWorkerMessage('ERROR', { message: err.message });
    }
}

let workerStaticTexturesStarted = false;
function preloadWorkerStaticEffectTextures() {
    if (workerStaticTexturesStarted || !wasmReady || !wasmModule || !gl) return;
    if (!wasmModule._wasm_render_alloc_texture || !wasmModule._wasm_render_bind_texture || !wasmModule._wasm_fx_register_static_texture) return;
    if (typeof fetch !== 'function' || typeof createImageBitmap !== 'function') return;
    workerStaticTexturesStarted = true;
    const specs = [
        { slot: 0, file: 'noise.png' },
        { slot: 1, file: 'RainStreaksTextureSheet.png' },
        { slot: 2, file: 'RainMap_Camera_Small_Medium.png' },
        { slot: 3, file: 'RainMap_Camera_Trail.png' }
    ];
    for (let i = 0; i < specs.length; i++) {
        const s = specs[i];
        fetch(`../Asset/texture/raindrop-pro/${s.file}`)
            .then(r => r.ok ? r.blob() : fetch(`Asset/texture/raindrop-pro/${s.file}`).then(r2 => r2.ok ? r2.blob() : null))
            .then(blob => blob ? createImageBitmap(blob, { premultiplyAlpha: 'none', colorSpaceConversion: 'none' }) : null)
            .then(bmp => {
                if (!bmp || !gl || !wasmModule) {
                    if (bmp && typeof bmp.close === 'function') bmp.close();
                    return;
                }
                const texId = wasmModule._wasm_render_alloc_texture();
                if (texId) {
                    wasmModule._wasm_render_bind_texture(texId);
                    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
                    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
                    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
                    wasmModule._wasm_fx_register_static_texture(s.slot, texId, bmp.width, bmp.height);
                }
                if (typeof bmp.close === 'function') bmp.close();
            })
            .catch(() => {});
    }
}

// 2. Initialize OffscreenCanvas WebGL2 Context
function initCanvas(canvas, width, height) {
    offscreenCanvas = canvas;
    if (width > 0) offscreenCanvas.width = width;
    if (height > 0) offscreenCanvas.height = height;

    const opt = {
        alpha: true,
        premultipliedAlpha: true,
        antialias: false,
        depth: false,
        stencil: false,
        preserveDrawingBuffer: true
    };
    gl = offscreenCanvas.getContext('webgl2', opt) || offscreenCanvas.getContext('webgl', opt);
    if (!gl) {
        postWorkerMessage('ERROR', { message: 'WebGL2 not supported in Worker' });
        return;
    }
    preloadWorkerStaticEffectTextures();
    postWorkerMessage('CANVAS_INITIALIZED', { width: offscreenCanvas.width, height: offscreenCanvas.height });
}

// 3. Render a single timeline frame inside Worker
function renderFrameInWorker(nowMs, width, height, sceneValid = 1, bgR = 0, bgG = 0, bgB = 0, bgA = 1) {
    if (!wasmReady || !wasmModule || !gl) return 0;
    if (offscreenCanvas.width !== width || offscreenCanvas.height !== height) {
        offscreenCanvas.width = width;
        offscreenCanvas.height = height;
    }

    const currentTick = ++frameTick;
    const fn = wasmModule._wasm_render_timeline_frame;
    if (!fn) return 0;

    const drawnCount = fn(
        Number(nowMs),
        width >>> 0,
        height >>> 0,
        currentTick >>> 0,
        sceneValid ? 1 : 0,
        Number(bgR),
        Number(bgG),
        Number(bgB),
        Number(bgA),
        1 // blitToScreen directly to OffscreenCanvas
    ) >>> 0;

    return drawnCount;
}

// 4. Zero-Allocation Shared Memory Telemetry & Command Dispatcher
function syncSharedTelemetry(drawnCount, seq = 0) {
    if (!consumer || !wasmModule || !wasmModule._wasm_get_telemetry_ptr) return;
    const ptr = wasmModule._wasm_get_telemetry_ptr();
    if (!ptr) return;
    const f64 = wasmModule.HEAPF64;
    const u32 = wasmModule.HEAPU32;
    const p8 = ptr >> 3;
    const p4 = ptr >> 2;
    const fps = f64[p8 + 0];
    const cpuMs = f64[p8 + 2];
    const hashLow = u32[p4 + 8];
    const hashHigh = u32[p4 + 9];
    const mode = u32[p4 + 11];
    const dirtyCount = u32[p4 + 12];
    const activeLayers = u32[p4 + 15];
    consumer.writeTelemetry(frameTick, fps, cpuMs, mode, hashLow, hashHigh, dirtyCount, activeLayers, drawnCount, seq);
}

function checkSharedChannel() {
    if (!consumer) return false;
    const cmd = consumer.pollLatestRenderCommand();
    if (cmd) {
        const drawn = renderFrameInWorker(
            cmd.nowMs,
            cmd.width,
            cmd.height,
            cmd.sceneValid,
            cmd.bgR, cmd.bgG, cmd.bgB, cmd.bgA
        );
        syncSharedTelemetry(drawn, cmd.seq);
        return true;
    }
    return false;
}

function runWorkerLoop(timestamp) {
    checkSharedChannel();

    if (isPlaying) {
        if (lastPlayTimestamp > 0) {
            const deltaMs = timestamp - lastPlayTimestamp;
            playbackTimeMs += deltaMs;
        }
        lastPlayTimestamp = timestamp;

        const w = offscreenCanvas ? offscreenCanvas.width : 1920;
        const h = offscreenCanvas ? offscreenCanvas.height : 1080;
        const drawn = renderFrameInWorker(playbackTimeMs, w, h, 1);
        syncSharedTelemetry(drawn, 0);
    }

    if (loopRunning) {
        if (typeof self.requestAnimationFrame === 'function') {
            self.requestAnimationFrame(runWorkerLoop);
        } else {
            setTimeout(() => runWorkerLoop(performance.now()), 8);
        }
    }
}

function rasterizeWorkerPath(it) {
    const d = it.d || it.path || (it.style && (it.style.d || it.style.path)) || '';
    if (!d) return null;
    const style = it.style || it.css || {};
    const overrides = it._nativeOverrides || {};
    const fill = overrides.fill || overrides.color || style.fill || style.color || it.fill || it.color || '#ffffff';
    const fillRule = (overrides['fill-rule'] || overrides.fillRule || style['fill-rule'] || style.fillRule || it['fill-rule'] || it.fillRule || 'nonzero');
    const stroke = overrides.stroke || overrides['stroke-color'] || overrides.strokeColor || style.stroke || it.stroke || 'none';
    const strokeWidth = parseFloat(overrides['stroke-width'] || overrides.strokeWidth || style['stroke-width'] || style.strokeWidth || it['stroke-width'] || it.strokeWidth || 0) || 0;
    const lineCap = overrides['stroke-linecap'] || overrides.strokeLinecap || style['stroke-linecap'] || style.strokeLinecap || it['stroke-linecap'] || it.strokeLinecap || 'butt';
    const lineJoin = overrides['stroke-linejoin'] || overrides.strokeLinejoin || style['stroke-linejoin'] || style.strokeLinejoin || it['stroke-linejoin'] || it.strokeLinejoin || 'miter';
    const viewBox = it.viewBox || (style && style.viewBox) || '';

    let cv = null;
    if (typeof OffscreenCanvas !== 'undefined') {
        cv = new OffscreenCanvas(512, 512);
    } else {
        return null;
    }
    const ctx = cv.getContext('2d');
    if (!ctx) return null;

    if (typeof Path2D !== 'undefined') {
        const pathObj = new Path2D(d);
        let x0 = 0, y0 = 0, w = 512, h = 512;
        let hasBounds = false;
        if (viewBox) {
            const parts = String(viewBox).trim().split(/[\s,]+/).map(Number);
            if (parts.length >= 4 && parts[2] > 0 && parts[3] > 0) {
                [x0, y0, w, h] = parts;
                hasBounds = true;
            }
        }
        if (!hasBounds) {
            let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
            const numRegex = /[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?/g;
            let match;
            let isX = true;
            while ((match = numRegex.exec(d)) !== null) {
                const val = parseFloat(match[0]);
                if (Number.isFinite(val)) {
                    if (isX) {
                        if (val < minX) minX = val;
                        if (val > maxX) maxX = val;
                    } else {
                        if (val < minY) minY = val;
                        if (val > maxY) maxY = val;
                    }
                    isX = !isX;
                }
            }
            if (Number.isFinite(minX) && Number.isFinite(maxX) && maxX > minX && maxY > minY) {
                x0 = minX; y0 = minY; w = maxX - minX; h = maxY - minY;
                hasBounds = true;
            }
        }

        ctx.save();
        if (hasBounds) {
            const pad = strokeWidth > 0 ? Math.min(32, Math.max(16, strokeWidth * 2)) : 16;
            const scale = Math.min((512 - pad * 2) / Math.max(w, 1), (512 - pad * 2) / Math.max(h, 1));
            const tx = pad + ((512 - pad * 2) - w * scale) / 2 - x0 * scale;
            const ty = pad + ((512 - pad * 2) - h * scale) / 2 - y0 * scale;
            ctx.translate(tx, ty);
            ctx.scale(scale, scale);
        }

        if (fill && fill !== 'none') {
            ctx.fillStyle = fill;
            ctx.fill(pathObj, fillRule);
        }
        if (stroke && stroke !== 'none' && strokeWidth > 0) {
            ctx.strokeStyle = stroke;
            ctx.lineWidth = strokeWidth;
            ctx.lineCap = lineCap;
            ctx.lineJoin = lineJoin;
            ctx.stroke(pathObj);
        }
        ctx.restore();
    }
    return cv;
}

function syncWorkerCompiledAssets() {
    if (!wasmModule || !gl) return;
    const getOutPtr = wasmModule._wasm_manager_get_compiled_items_json_ptr;
    const getOutLen = wasmModule._wasm_manager_get_compiled_items_json_length;
    if (!getOutPtr || !getOutLen) return;
    const outPtr = getOutPtr();
    const outLen = getOutLen();
    if (!outPtr || outLen === 0) return;

    try {
        const jsonStr = new TextDecoder().decode(wasmModule.HEAPU8.subarray(outPtr, outPtr + outLen));
        const items = JSON.parse(jsonStr);
        if (!Array.isArray(items)) return;

        for (let i = 0; i < items.length; ++i) {
            const it = items[i];
            if (!it) continue;
            const slot = it._nativeSlot !== undefined ? it._nativeSlot : i;
            if (it.type === 'path') {
                const pathCanvas = rasterizeWorkerPath(it);
                if (pathCanvas) {
                    let texId = slotTexMap[slot];
                    if (!texId && wasmModule._wasm_render_alloc_texture) {
                        texId = wasmModule._wasm_render_alloc_texture();
                        slotTexMap[slot] = texId;
                    }
                    if (texId && wasmModule._wasm_render_bind_texture) {
                        wasmModule._wasm_render_bind_texture(texId);
                        gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
                        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, pathCanvas);
                        if (wasmModule._wasm_render_set_slot_texture) {
                            wasmModule._wasm_render_set_slot_texture(slot, texId);
                        }
                    }
                }
            }
        }
    } catch (e) {
        console.warn('[RenderWorker] syncWorkerCompiledAssets error:', e);
    }
}

// 5. Message Dispatcher
self.onmessage = async function (e) {
    const data = e.data;
    if (!data || !data.type) return;

    switch (data.type) {
        case 'INIT': {
            if (data.canvas) {
                initCanvas(data.canvas, data.width || 1920, data.height || 1080);
            }
            if (data.sab) {
                if (typeof SharedChannel === 'undefined' && typeof importScripts === 'function') {
                    try {
                        importScripts('shared-channel.js');
                    } catch (e1) {
                        try {
                            importScripts('../js/shared-channel.js');
                        } catch (e2) {}
                    }
                }
                if (typeof SharedChannel !== 'undefined' && SharedChannel.SharedChannelConsumer) {
                    consumer = new SharedChannel.SharedChannelConsumer(data.sab);
                }
            }
            await initWasmInWorker(data.wasmBinary || null);
            if (consumer && !loopRunning) {
                loopRunning = true;
                if (typeof self.requestAnimationFrame === 'function') {
                    self.requestAnimationFrame(runWorkerLoop);
                } else {
                    setTimeout(() => runWorkerLoop(performance.now()), 8);
                }
            }
            break;
        }
        case 'RESIZE': {
            if (offscreenCanvas && data.width > 0 && data.height > 0) {
                offscreenCanvas.width = data.width;
                offscreenCanvas.height = data.height;
                if (gl) gl.viewport(0, 0, data.width, data.height);
            }
            break;
        }
        case 'RENDER_FRAME': {
            const drawn = renderFrameInWorker(
                data.nowMs || 0,
                data.width || (offscreenCanvas ? offscreenCanvas.width : 1920),
                data.height || (offscreenCanvas ? offscreenCanvas.height : 1080),
                data.sceneValid !== undefined ? data.sceneValid : 1,
                data.bgR || 0,
                data.bgG || 0,
                data.bgB || 0,
                data.bgA !== undefined ? data.bgA : 1
            );
            syncSharedTelemetry(drawn, 0);
            if (data.needAck) {
                postWorkerMessage('FRAME_DRAWN', { nowMs: data.nowMs, drawn });
            }
            break;
        }
        case 'UPDATE_VIDEO_BITMAP': {
            // Zero-Copy ImageBitmap upload directly inside Worker
            const { slot, resId, bitmap } = data;
            if (gl && bitmap) {
                let texId = slotTexMap[slot];
                if (!texId && wasmModule && wasmModule._wasm_render_alloc_texture) {
                    texId = wasmModule._wasm_render_alloc_texture();
                    slotTexMap[slot] = texId;
                    if (wasmModule._wasm_render_set_slot_texture) {
                        wasmModule._wasm_render_set_slot_texture(slot, texId);
                    }
                    if (resId && wasmModule._wasm_render_set_resource_texture) {
                        wasmModule._wasm_render_set_resource_texture(resId, texId);
                    }
                }
                if (texId && wasmModule && wasmModule._wasm_render_bind_texture) {
                    wasmModule._wasm_render_bind_texture(texId);
                    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
                    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
                }
                if (typeof bitmap.close === 'function') {
                    bitmap.close();
                }
            }
            break;
        }
        case 'START_PLAYBACK': {
            isPlaying = true;
            playbackTimeMs = data.startTimeMs || 0;
            lastPlayTimestamp = performance.now();
            if (!loopRunning) {
                loopRunning = true;
                if (typeof self.requestAnimationFrame === 'function') {
                    self.requestAnimationFrame(runWorkerLoop);
                } else {
                    runWorkerLoop(performance.now());
                }
            }
            break;
        }
        case 'STOP_PLAYBACK': {
            isPlaying = false;
            lastPlayTimestamp = 0;
            break;
        }
        case 'SET_PROJECT_JSON': {
            if (wasmModule && data.jsonString) {
                const encoder = new TextEncoder();
                const bytes = encoder.encode(data.jsonString);
                const getPtr = wasmModule._wasm_manager_get_project_json_ptr;
                const loadFn = wasmModule._wasm_manager_load_project_json;
                const getCap = wasmModule._wasm_manager_get_project_json_capacity;
                const cap = getCap ? getCap() : 1048576;
                const bufPtr = getPtr ? getPtr() : 0;
                if (bufPtr && loadFn && bytes.length <= cap) {
                    wasmModule.HEAPU8.set(bytes, bufPtr);
                    loadFn(bytes.length, data.stageW || 1920, data.stageH || 1080);
                    syncWorkerCompiledAssets();
                    postWorkerMessage('PROJECT_UPDATED', { length: bytes.length });
                }
            }
            break;
        }
        case 'RESET_PROJECT': {
            if (wasmModule && wasmModule._wasm_render_reset_project_state) {
                wasmModule._wasm_render_reset_project_state();
            }
            break;
        }
        default:
            break;
    }
};
