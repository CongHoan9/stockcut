// ============================================================================
// js/wasm-bridge.js
// Cáº§u ná»‘i WebAssembly (Zero-Copy Shared Memory Bridge) & Transparent Fallback
// Tinh hoa Stockfish: CÄƒn chá»‰nh bá»™ nhá»› cacheline, 64-bit ALU, SIMD Vectorization
// ============================================================================
(function (global) {
    'use strict';
    // Polynomial string hash trÃ¹ng khá»›p 100% vá»›i timeline-engine.js
    function strHash(s) {
        if (!s) return 0;
        let h = 0;
        for (let i = 0; i < s.length; i++) {
            h = ((h << 5) - h + s.charCodeAt(i)) | 0;
        }
        return h;
    }
    // JS tier (0 neutral,1 static,2 finite,3 continuous) -> C++ MediaTier (0 Static,1 Finite,2 Continuous)
    const NATIVE_TIER = Uint8Array.of(0, 0, 1, 2);
    const effectZoomResult = new Float32Array(3);
    const effectShakeResult = new Float32Array(4);
    const STATE = {
        ready: false,
        instance: null,
        memory: null,
        views: {
            f64: null,
            f32: null,
            u32: null,
            u8: null,
            u64: null
        },
        exports: null,
        dirtyBufPtr: 0,
        areaPtr: 0,
        effectScratchPtr: 0,
        computationCacheInfoPtr: 0,
        magBufPtr: 0,
        sampleBufPtr: 0,
        itemKeysBufPtr: 0,
        itemKeysCapacity: 0,
        projectJsonPtr: 0,
        projectJsonCapacity: 0,
        projectJsonView: null,
        projectDurationResultPtr: 0,
        compiledItemsJsonPtr: 0,
        vboInputBufferPtr: 0,
        vboInputView: null,
        vboRenderConstants: null,
        compositeEffectOrderInputPtr: 0,
        compositeEffectOrderOutputPtr: 0,
        compositeEffectOrderInputFloatView: null,
        compositeEffectOrderInputUintView: null,
        compositeEffectOrderOutputView: null,
        compositeCssColorBufferPtr: 0,
        compositeCssColorResultPtr: 0,
        compositeCssColorBufferView: null,
        compositeCssColorResultView: null,
        timelineSetupInputBufferPtr: 0,
        timelineSetupInputCapacity: 0,
        timelineSetupInputView: null,
        timelineGeometryResultPtr: 0,
        timelineGeometryResultView: null,
        timelineTextStyleResultPtr: 0,
        timelineTextStyleResultView: null,
        timelineTimeResultPtr: 0,
        timelineTimeResultView: null,
        telemetryPtr: 0,
        displayListPtr: 0,
        pipelineResultPtr: 0,
        stylesPtr: 0,
        textInputBufPtr: 0,
        textGlyphsPtr: 0,
        textResultPtr: 0,
        nativeFontInputPtr: 0,
        nativeFontInputCapacity: 0,
        nativeFontUtf8Ptr: 0,
        nativeFontUtf8Capacity: 0,
        nativeFontGlyphsPtr: 0,
        nativeFontGlyphCapacity: 0,
        nativeFontAtlasPtr: 0,
        nativeFontDirtyRectPtr: 0,
        nativeFontKey: 0,
        nativeFontViews: null,
        textRevealBufferPtr: 0,
        textRevealCapacity: 0,
        textRevealSegmentCapacity: 0,
        textRevealFlagNever: 0,
        textRevealFlagTilde: 0,
        textRevealViews: null,
        geometryNodesPtr: 0,
        geometryResultPtr: 0,
        filterArenaPtr: 0,
        audioVolumeEnvelopesPtr: 0,
        currentBuf: null,
        mode: 'WASM_SIMD'
    };
    function writeTimelineSetupText(value, offset) {
        if (value === undefined || value === null) return 0;
        const text = String(value);
        const length = text.length;
        if (offset + length > STATE.timelineSetupInputView.length) {
            throw new RangeError('[WasmBridge] Timeline geometry CSS input exceeds native buffer capacity.');
        }
        for (let i = 0; i < length; i++) {
            const code = text.charCodeAt(i);
            STATE.timelineSetupInputView[offset + i] =
                code <= 0x7F ? code : 0x3F;
        }
        return length;
    }
    // Transposition Table DisplayList Command Buffer Cache Arena (JS Fallback)
    const _jsDlArena = new Array(64);
    let _jsDlArenaNext = 0;
    // ========================================================================
    // STOCKFISH WASM CORE BINARY (Standalone C++20 SIMD128 Module)
    // Tá»± tÃºc nhá»‹ phÃ¢n (Zero external CORS dependency) â€” Cháº¡y Ä‘Æ°á»£c cáº£ trÃªn file://
    // ========================================================================
    const EMBEDDED_WASM_B64 = "";
    // ========================================================================
    // KHá»žI Äá»˜NG WEBASSEMBLY (Há»— trá»£ Standalone Memory, Imported Memory & B64 Fallback)
    let _initPromise = null;
    function initWasm(wasmUrl = 'js/stockcut.wasm') {
        if (_initPromise) return _initPromise;
        _initPromise = (async () => {
            try {
                let runtimeFactory = global.createStockcutModule;
                if (typeof runtimeFactory !== 'function') {
                    const runtimeUrls = [
                        'js/stockcut_runtime.js',
                        '../js/stockcut_runtime.js',
                        '/js/stockcut_runtime.js'
                    ];
                    let runtimeLoadError = null;
                    for (const url of runtimeUrls) {
                        try {
                            await new Promise((resolve, reject) => {
                                const script = document.createElement('script');
                                script.src = url;
                                script.async = true;
                                script.onload = resolve;
                                script.onerror = () => reject(new Error(`Unable to load Emscripten runtime: ${url}`));
                                (document.head || document.documentElement).appendChild(script);
                            });
                            runtimeFactory = global.createStockcutModule;
                            if (typeof runtimeFactory === 'function') break;
                        } catch (error) {
                            runtimeLoadError = error;
                        }
                    }
                    if (typeof runtimeFactory !== 'function') {
                        const detail = runtimeLoadError ? ` ${runtimeLoadError.message}` : '';
                        throw new Error(`Emscripten runtime factory is unavailable.${detail}`);
                    }
                }
                const candidateUrls = [
                    wasmUrl,
                    'js/stockcut.wasm',
                    '/js/stockcut.wasm',
                    'native/stockcut.wasm',
                    '/native/stockcut.wasm'
                ];
                const module = await runtimeFactory({
                    noInitialRun: true,
                    instantiateWasm(imports, successCallback) {
                        (async () => {
                            for (const url of candidateUrls) {
                                if (!url) continue;
                                try {
                                    const resp = await fetch(url);
                                    if (!resp || !resp.ok) continue;
                                    if (typeof WebAssembly.instantiateStreaming === 'function') {
                                        try {
                                            const streamed = await WebAssembly.instantiateStreaming(Promise.resolve(resp.clone()), imports);
                                            successCallback(streamed.instance, streamed.module);
                                            return;
                                        } catch (_) {}
                                    }
                                    const buf = await resp.arrayBuffer();
                                    const inst = await WebAssembly.instantiate(buf, imports);
                                    successCallback(inst.instance, inst.module);
                                    return;
                                } catch (_) {}
                            }
                            if (EMBEDDED_WASM_B64) {
                                const binStr = atob(EMBEDDED_WASM_B64);
                                const u8 = new Uint8Array(binStr.length);
                                for (let i = 0; i < binStr.length; i++) u8[i] = binStr.charCodeAt(i);
                                const inst = await WebAssembly.instantiate(u8, imports);
                                successCallback(inst.instance, inst.module);
                                return;
                            }
                            throw new Error('[WasmBridge FATAL] Missing stockcut.wasm binary');
                        })().catch(err => console.error('[WasmBridge] instantiateWasm error:', err));
                        return {};
                    }
                });
                if (!module || typeof module !== 'object') {
                    throw new Error('Emscripten runtime did not return a module instance.');
                }
                STATE.instance = module;
                STATE.exports = module;
                if (!module.wasmMemory && (!module.HEAPU8 || !module.HEAPU8.buffer)) {
                    throw new Error('Emscripten runtime did not expose linear memory.');
                }
                STATE.memory = module.wasmMemory || {
                    get buffer() {
                        return module.HEAPU8.buffer;
                    }
                };
                if (!STATE.memory) {
                    throw new Error('Module WebAssembly khÃ´ng cung cáº¥p linear memory.');
                }
                updateMemoryViews();
                // Khá»Ÿi táº¡o Zobrist Tables & FFT Tables trong Wasm
                const initZobrist = getExport('wasm_zobrist_init');
                if (initZobrist) initZobrist(0x9e3779b97f4a7c15n);
                const initFft = getExport('wasm_fft_init');
                if (initFft) initFft();
                // Truy váº¥n cÃ¡c con trá» tÄ©nh Ä‘Ã£ cáº¥p phÃ¡t tá»« C++ Wasm
                STATE.dirtyBufPtr = callGetter('wasm_get_dirty_buf');
                STATE.areaPtr = callGetter('wasm_get_area_ptr');
                STATE.effectScratchPtr = callGetter('wasm_get_effect_scratch_ptr');
                STATE.computationCacheInfoPtr = callGetter('wasm_get_computation_cache_info_buf');
                STATE.magBufPtr = callGetter('wasm_get_mag_buf');
                STATE.sampleBufPtr = callGetter('wasm_get_sample_buf');
                STATE.itemKeysBufPtr = callGetter('wasm_get_item_keys_buf');
                STATE.itemKeysCapacity = callGetter('wasm_get_item_keys_capacity');
                STATE.telemetryPtr = callGetter('wasm_get_telemetry_ptr');
                STATE.displayListPtr = callGetter('wasm_get_display_list_ptr');
                STATE.pipelineResultPtr = callGetter('wasm_get_pipeline_result_ptr');
                STATE.stylesPtr = callGetter('wasm_get_styles_ptr');
                STATE.textInputBufPtr = callGetter('wasm_text_get_input_buf');
                STATE.textGlyphsPtr = callGetter('wasm_text_get_glyphs_ptr');
                STATE.textResultPtr = callGetter('wasm_text_get_result_ptr');
                STATE.nativeFontInputPtr = callGetter('wasm_text_font_get_input_buffer_ptr');
                STATE.nativeFontInputCapacity = callGetter('wasm_text_font_get_input_capacity');
                STATE.nativeFontUtf8Ptr = callGetter('wasm_text_font_get_utf8_buffer_ptr');
                STATE.nativeFontUtf8Capacity = callGetter('wasm_text_font_get_utf8_capacity');
                STATE.nativeFontGlyphsPtr = callGetter('wasm_text_font_get_glyph_buffer_ptr');
                STATE.nativeFontGlyphCapacity = callGetter('wasm_text_font_get_glyph_capacity');
                STATE.nativeFontAtlasPtr = callGetter('wasm_text_font_get_atlas_ptr');
                STATE.nativeFontDirtyRectPtr =
                    callGetter('wasm_text_font_get_atlas_dirty_rect_ptr');
                STATE.textRevealBufferPtr = callGetter('wasm_text_reveal_get_buffer_ptr');
                STATE.textRevealCapacity = callGetter('wasm_text_reveal_get_capacity');
                STATE.textRevealSegmentCapacity =
                    callGetter('wasm_text_reveal_get_segment_capacity');
                STATE.textRevealFlagNever = callGetter('wasm_text_reveal_get_never_flag');
                STATE.textRevealFlagTilde = callGetter('wasm_text_reveal_get_tilde_flag');
                STATE.geometryNodesPtr = callGetter('wasm_geometry_get_nodes_ptr');
                STATE.geometryResultPtr = callGetter('wasm_geometry_get_result_ptr');
                STATE.filterArenaPtr = callGetter('wasm_filter_get_arena');
                STATE.kawasePassesPtr = callGetter('wasm_filter_get_kawase_passes_buf');
                STATE.audioSyncResultPtr = callGetter('wasm_audio_get_sync_result_ptr');
                STATE.audioVolumeEnvelopesPtr = callGetter('wasm_audio_get_volume_envelopes_ptr');
                STATE.sharedStatePtr = callGetter('wasm_get_shared_state_ptr');
                STATE.projectJsonPtr = callGetter('wasm_manager_get_project_json_ptr');
                STATE.projectJsonCapacity = callGetter('wasm_manager_get_project_json_capacity');
                STATE.projectDurationResultPtr = callGetter('wasm_manager_get_project_duration_result_ptr');
                STATE.compiledItemsJsonPtr =
                    callGetter('wasm_manager_get_compiled_items_json_ptr');
                STATE.compositeEffectOrderInputPtr =
                    callGetter('wasm_manager_get_composite_effect_order_input_buffer');
                STATE.compositeEffectOrderOutputPtr =
                    callGetter('wasm_manager_get_composite_effect_order_output_buffer');
                STATE.compositeCssColorBufferPtr =
                    callGetter('wasm_manager_get_composite_css_color_buffer');
                STATE.compositeCssColorResultPtr =
                    callGetter('wasm_manager_get_composite_css_color_result_buffer');
                STATE.timelineSetupInputBufferPtr =
                    callGetter('wasm_manager_get_timeline_setup_input_buffer');
                STATE.timelineSetupInputCapacity =
                    callGetter('wasm_manager_get_timeline_setup_input_capacity');
                STATE.timelineGeometryResultPtr =
                    callGetter('wasm_manager_get_timeline_geometry_result_buffer');
                STATE.timelineTextStyleResultPtr =
                    callGetter('wasm_manager_get_timeline_text_style_result_buffer');
                STATE.timelineTimeResultPtr =
                    callGetter('wasm_manager_get_timeline_time_result_buffer');
                updateMemoryViews();
                STATE.ready = true;
                STATE.mode = 'WASM_SIMD';
                console.log('[WasmBridge] [+] Khởi tạo thành công Stockfish WebAssembly Core Engine (WASM-SIMD128)!');
                if (global.NativeComputeCache) {
                    global.NativeComputeCache.requestPersistentStorage();
                }
                if (typeof window !== 'undefined' && typeof window.dispatchEvent === 'function') {
                    window.dispatchEvent(new CustomEvent('wasm-ready', { detail: { mode: STATE.mode } }));
                }
                return true;
            } catch (e) {
                console.error('[WasmBridge FATAL] Khá»Ÿi táº¡o WebAssembly tháº¥t báº¡i:', e.message);
                STATE.ready = false;
                STATE.mode = 'CRASHED';
                throw new Error('[WasmBridge FATAL] WebAssembly Core khÃ´ng thá»ƒ khá»Ÿi táº¡o: ' + e.message);
            }
        })();
        return _initPromise;
    }
    function getExport(name) {
        if (!STATE.exports) {
            throw new Error('[WasmBridge FATAL] WebAssembly Core chÆ°a sáºµn sÃ ng. HÃ£y gá»i await WasmBridge.whenReady() trÆ°á»›c khi dÃ¹ng API!');
        }
        const fn = STATE.exports[name] || STATE.exports['_' + name];
        if (!fn) {
            throw new Error('[WasmBridge FATAL] Thiáº¿u hÃ m C++ Wasm: ' + name);
        }
        return fn;
    }
    function getOptionalExport(name) {
        if (!STATE.exports) return null;
        return STATE.exports[name] || STATE.exports['_' + name] || null;
    }
    function callGetter(name) {
        const fn = getExport(name);
        return fn ? fn() : 0;
    }
    // ========================================================================
    // SHARED LINEAR MEMORY TYPEDARRAY VIEWS (ZERO-COPY CORE-DOM)
    // ========================================================================
    const telemetryViews = {
        buffer: null,
        ptr: 0,
        f64: null,
        f32: null,
        u32: null,
        u8: null,
        u64: null
    };
    let dlCount = 0;
    const displayListViews = {
        buffer: null,
        ptr: 0,
        u32: null,
        u16: null,
        f32: null
    };
    const stylesViews = {
        buffer: null,
        ptr: 0,
        f32: null,
        u32: null,
        i32: null,
        u8: null
    };
    const textInputViews = { buffer: null, ptr: 0, u32: null };
    const textGlyphsViews = { buffer: null, ptr: 0, f32: null, u32: null };
    const textResultViews = { buffer: null, ptr: 0, f32: null, u32: null };
    const nativeFontViews = {
        buffer: null, input: null, utf8: null, glyphsF32: null, glyphsU32: null,
        atlas: null, dirtyRect: null
    };
    const geometryNodesViews = { buffer: null, ptr: 0, f32: null, u32: null, u16: null };
    const geometryResultViews = { buffer: null, ptr: 0, u32: null, f32: null };
    const filterViews = { buffer: null, ptr: 0, f32: null, u32: null };
    const kawasePassesViews = { buffer: null, ptr: 0, f32: null, u32: null };
    const sharedStateViews = {
        buffer: null,
        ptr: 0,
        dataView: null,
        f64: null,
        u32: null,
        f32: null
    };
    const RENDER_MODE_CODES = {
        DIRTY: 0,
        FULL: 1,
        ZOBRIST_SKIP: 2,
        TT_HIT: 3,
        NONE: 4
    };
    const RENDER_MODE_NAMES = ['DIRTY', 'FULL', 'ZOBRIST_SKIP', 'TT_HIT', 'NONE'];
    function isBufferDetached(buf) {
        if (!buf) return true;
        try {
            return buf.byteLength === 0;
        } catch (_) {
            return true;
        }
    }
    function ensureValidMemory() {
        if (!STATE.ready || !STATE.memory) {
            throw new Error('[WasmBridge FATAL] WebAssembly linear memory is not available!');
        }
        const currentBuf = STATE.memory.buffer;
        if (!STATE.currentBuf || STATE.currentBuf !== currentBuf || isBufferDetached(STATE.currentBuf)) {
            STATE.currentBuf = currentBuf;
            updateMemoryViews();
            return true;
        }
        return false;
    }
    // Queues validated native misses for asynchronous IndexedDB lookup/import.
    function forwardEffectCacheMiss() {
        const exports = STATE.exports || {};
        const infoFn = exports.wasm_computation_cache_last_info || exports._wasm_computation_cache_last_info;
        const payloadFn = exports.wasm_computation_cache_last_payload || exports._wasm_computation_cache_last_payload;
        const transferFn = exports.wasm_computation_cache_transfer_buffer || exports._wasm_computation_cache_transfer_buffer;
        const importFn = exports.wasm_computation_cache_import || exports._wasm_computation_cache_import;
        const persistResultFn = exports.wasm_computation_cache_persist_result ||
            exports._wasm_computation_cache_persist_result;
        const adapter = global.NativeComputeCache;
        if (!infoFn || !payloadFn || !transferFn || !importFn || !adapter ||
            !STATE.computationCacheInfoPtr) return;
        ensureValidMemory();
        infoFn(STATE.computationCacheInfoPtr);
        ensureValidMemory();
        const infoOffset = STATE.computationCacheInfoPtr >> 2;
        const info = STATE.views.u32;
        const keyLow = info[infoOffset] >>> 0;
        const keyHigh = info[infoOffset + 1] >>> 0;
        const domain = info[infoOffset + 2] >>> 0;
        const size = info[infoOffset + 3] >>> 0;
        const checksum = info[infoOffset + 4] >>> 0;
        const hit = info[infoOffset + 5] === 1;
        const persist = info[infoOffset + 6] === 1;
        if (!size || (hit && !persist)) return;
        const payloadPtr = payloadFn();
        ensureValidMemory();
        const payload = STATE.views.u8.subarray(payloadPtr, payloadPtr + size);
        adapter.handleNativeMiss(
            { keyLow, keyHigh, domain, size, checksum, hit, persist },
            payload,
            (low, high, recordDomain, recordSize, recordChecksum, bytes) => {
                ensureValidMemory();
                const transferPtr = transferFn();
                STATE.views.u8.set(bytes, transferPtr);
                return importFn(
                    low, high, recordDomain, recordSize, recordChecksum, transferPtr
                ) === 1;
            },
            (succeeded) => {
                if (!persistResultFn) return;
                ensureValidMemory();
                persistResultFn(keyLow, keyHigh, domain, succeeded ? 1 : 0);
            }
        );
    }
    function updateMemoryViews() {
        if (!STATE.memory) return;
        const buf = STATE.memory.buffer;
        STATE.currentBuf = buf;
        STATE.views.f64 = new Float64Array(buf);
        STATE.views.f32 = new Float32Array(buf);
        STATE.views.u32 = new Uint32Array(buf);
        STATE.views.u8 = new Uint8Array(buf);
        if (!STATE.vboRenderConstants && STATE.exports) {
            const countFn = STATE.exports.wasm_vbo_get_render_constants_count ||
                STATE.exports._wasm_vbo_get_render_constants_count;
            const dataFn = STATE.exports.wasm_vbo_get_render_constants_ptr ||
                STATE.exports._wasm_vbo_get_render_constants_ptr;
            if (countFn && dataFn) {
                const values = new Uint32Array(buf, dataFn(), countFn());
                STATE.vboRenderConstants = {
                    maxBatchQuads: values[0],
                    floatsPerVertex: values[1],
                    verticesPerQuad: values[2],
                    floatsPerQuad: values[3],
                    floatsPerQuadInput: values[4],
                    maxTextureSlots: values[5],
                    floatBytes: values[6],
                    vertexPositionComponents: values[7],
                    vertexUvComponents: values[8],
                    vertexColorComponents: values[9],
                    vertexTextureIdComponents: values[10],
                    vertexModeComponents: values[11],
                    vertexSpinComponents: values[12],
                    vertexPositionOffset: values[13],
                    vertexUvOffset: values[14],
                    vertexColorOffset: values[15],
                    vertexTextureIdOffset: values[16],
                    vertexModeOffset: values[17],
                    vertexSpinOffset: values[18],
                    maxDisplayCommands: values[19],
                    drawCommandWords: values[20],
                    drawCommandFloats: values[21],
                    renderCommandWords: values[22],
                    renderCommandFloats: values[23],
                    vboInputOffsets: Array.from(values.subarray(24, 42))
                };
            }
        }
        if (STATE.vboInputBufferPtr) {
            STATE.vboInputView = new Float32Array(
                buf,
                STATE.vboInputBufferPtr,
                STATE.vboRenderConstants.maxBatchQuads *
                STATE.vboRenderConstants.floatsPerQuadInput
            );
        }
        if (STATE.projectJsonPtr && STATE.projectJsonCapacity) {
            STATE.projectJsonView = new Uint8Array(
                buf,
                STATE.projectJsonPtr,
                STATE.projectJsonCapacity
            );
        }
        if (STATE.textRevealBufferPtr && STATE.textRevealCapacity) {
            const count = STATE.textRevealCapacity;
            const bytePtr = STATE.textRevealBufferPtr;
            const doubleBytes = count * Float64Array.BYTES_PER_ELEMENT;
            const uintBytes = count * Uint32Array.BYTES_PER_ELEMENT;
            const segmentCount = STATE.textRevealSegmentCapacity;
            const segmentBytes = segmentCount * Uint32Array.BYTES_PER_ELEMENT;
            const mapOffset = bytePtr + doubleBytes * 5 + uintBytes * 2;
            STATE.textRevealViews = {
                t0: new Float64Array(buf, bytePtr, count),
                fade: new Float64Array(buf, bytePtr + doubleBytes, count),
                dieT: new Float64Array(buf, bytePtr + doubleBytes * 2, count),
                dieFade: new Float64Array(buf, bytePtr + doubleBytes * 3, count),
                alpha: new Float64Array(buf, bytePtr + doubleBytes * 4, count),
                flags: new Uint32Array(buf, bytePtr + doubleBytes * 5, count),
                visible: new Uint32Array(buf, bytePtr + doubleBytes * 5 + uintBytes, count),
                layoutCodepoints: new Uint32Array(buf, mapOffset, count),
                segmentCodepoints: new Uint32Array(buf, mapOffset + uintBytes, count),
                segmentOffsets: new Uint32Array(
                    buf, mapOffset + uintBytes * 2, segmentCount
                ),
                segmentLengths: new Uint32Array(
                    buf, mapOffset + uintBytes * 2 + segmentBytes, segmentCount
                ),
                segmentAt: new Float64Array(
                    buf, mapOffset + uintBytes * 2 + segmentBytes * 2, segmentCount
                ),
                owner: new Int32Array(
                    buf, mapOffset + uintBytes * 2 + segmentBytes * 2 +
                segmentCount * Float64Array.BYTES_PER_ELEMENT, count
                ),
                jpos: new Uint32Array(
                    buf, mapOffset + uintBytes * 2 + segmentBytes * 2 +
                    segmentCount * Float64Array.BYTES_PER_ELEMENT + uintBytes, count
                )
            };
        }
        if (STATE.compositeEffectOrderInputPtr && STATE.compositeEffectOrderOutputPtr) {
            STATE.compositeEffectOrderInputFloatView = new Float64Array(
                buf, STATE.compositeEffectOrderInputPtr, 64 * 3
            );
            STATE.compositeEffectOrderInputUintView = new Uint32Array(
                buf, STATE.compositeEffectOrderInputPtr, 64 * 6
            );
            STATE.compositeEffectOrderOutputView = new Uint32Array(
                buf, STATE.compositeEffectOrderOutputPtr, 64
            );
        }
        if (STATE.compositeCssColorBufferPtr) {
            STATE.compositeCssColorBufferView = new Uint8Array(
                buf,
                STATE.compositeCssColorBufferPtr,
                256
            );
        }
        if (STATE.compositeCssColorResultPtr) {
            STATE.compositeCssColorResultView = new Float64Array(
                buf,
                STATE.compositeCssColorResultPtr,
                4
            );
        }
        if (STATE.timelineSetupInputBufferPtr) {
            STATE.timelineSetupInputView = new Uint8Array(
                buf,
                STATE.timelineSetupInputBufferPtr,
                STATE.timelineSetupInputCapacity
            );
        }
        if (STATE.timelineGeometryResultPtr) {
            STATE.timelineGeometryResultView = new Float64Array(
                buf,
                STATE.timelineGeometryResultPtr,
                6
            );
        }
        if (STATE.timelineTextStyleResultPtr) {
            STATE.timelineTextStyleResultView = new Uint32Array(
                buf,
                STATE.timelineTextStyleResultPtr,
                6
            );
        }
        if (STATE.timelineTimeResultPtr) {
            STATE.timelineTimeResultView = new Float64Array(
                buf,
                STATE.timelineTimeResultPtr,
                2
            );
        }
        if (typeof BigUint64Array !== 'undefined') {
            STATE.views.u64 = new BigUint64Array(buf);
        }
        if (STATE.telemetryPtr) {
            telemetryViews.buffer = buf;
            telemetryViews.ptr = STATE.telemetryPtr;
            telemetryViews.f64 = new Float64Array(buf, STATE.telemetryPtr, 32);
            telemetryViews.f32 = new Float32Array(buf, STATE.telemetryPtr, 64);
            telemetryViews.u32 = new Uint32Array(buf, STATE.telemetryPtr, 64);
            telemetryViews.u8 = new Uint8Array(buf, STATE.telemetryPtr, 256);
            if (typeof BigUint64Array !== 'undefined') {
                telemetryViews.u64 = new BigUint64Array(buf, STATE.telemetryPtr, 32);
            }
        }
        if (STATE.displayListPtr) {
            displayListViews.buffer = buf;
            displayListViews.ptr = STATE.displayListPtr;
            displayListViews.u32 = new Uint32Array(buf, STATE.displayListPtr, 2048);
            displayListViews.u16 = new Uint16Array(buf, STATE.displayListPtr, 4096);
            displayListViews.f32 = new Float32Array(buf, STATE.displayListPtr, 2048);
        }
        if (STATE.stylesPtr) {
            stylesViews.buffer = buf;
            stylesViews.ptr = STATE.stylesPtr;
            stylesViews.f32 = new Float32Array(buf, STATE.stylesPtr, 8192);
            stylesViews.u32 = new Uint32Array(buf, STATE.stylesPtr, 8192);
            stylesViews.i32 = new Int32Array(buf, STATE.stylesPtr, 8192);
            stylesViews.u8 = new Uint8Array(buf, STATE.stylesPtr, 32768);
        }
        if (STATE.textInputBufPtr) {
            textInputViews.buffer = buf;
            textInputViews.ptr = STATE.textInputBufPtr;
            textInputViews.u32 = new Uint32Array(buf, STATE.textInputBufPtr, 4096);
        }
        if (STATE.textGlyphsPtr) {
            textGlyphsViews.buffer = buf;
            textGlyphsViews.ptr = STATE.textGlyphsPtr;
            textGlyphsViews.f32 = new Float32Array(buf, STATE.textGlyphsPtr, 2048 * 8);
            textGlyphsViews.u32 = new Uint32Array(buf, STATE.textGlyphsPtr, 2048 * 8);
        }
        if (STATE.textResultPtr) {
            textResultViews.buffer = buf;
            textResultViews.ptr = STATE.textResultPtr;
            textResultViews.f32 = new Float32Array(buf, STATE.textResultPtr, 16);
            textResultViews.u32 = new Uint32Array(buf, STATE.textResultPtr, 16);
        }
        if (STATE.geometryNodesPtr) {
            geometryNodesViews.buffer = buf;
            geometryNodesViews.ptr = STATE.geometryNodesPtr;
            geometryNodesViews.f32 = new Float32Array(buf, STATE.geometryNodesPtr, 256 * 16);
            geometryNodesViews.u32 = new Uint32Array(buf, STATE.geometryNodesPtr, 256 * 16);
            geometryNodesViews.u16 = new Uint16Array(buf, STATE.geometryNodesPtr, 256 * 32);
        }
        if (STATE.geometryResultPtr) {
            geometryResultViews.buffer = buf;
            geometryResultViews.ptr = STATE.geometryResultPtr;
            geometryResultViews.u32 = new Uint32Array(buf, STATE.geometryResultPtr, 16);
            geometryResultViews.f32 = new Float32Array(buf, STATE.geometryResultPtr, 16);
        }
        if (STATE.filterArenaPtr) {
            filterViews.buffer = buf;
            filterViews.ptr = STATE.filterArenaPtr;
            filterViews.f32 = new Float32Array(buf, STATE.filterArenaPtr, 256 * 16);
            filterViews.u32 = new Uint32Array(buf, STATE.filterArenaPtr, 256 * 16);
        }
        if (STATE.kawasePassesPtr) {
            kawasePassesViews.buffer = buf;
            kawasePassesViews.ptr = STATE.kawasePassesPtr;
            kawasePassesViews.f32 = new Float32Array(buf, STATE.kawasePassesPtr, 8 * 4);
            kawasePassesViews.u32 = new Uint32Array(buf, STATE.kawasePassesPtr, 8 * 4);
        }
        if (STATE.sharedStatePtr) {
            sharedStateViews.buffer = buf;
            sharedStateViews.ptr = STATE.sharedStatePtr;
            sharedStateViews.dataView = new DataView(buf, STATE.sharedStatePtr, 128);
            sharedStateViews.f64 = new Float64Array(buf, STATE.sharedStatePtr, 16);
            sharedStateViews.u32 = new Uint32Array(buf, STATE.sharedStatePtr, 32);
            sharedStateViews.f32 = new Float32Array(buf, STATE.sharedStatePtr, 32);
        }
        if (STATE.nativeFontInputPtr && STATE.nativeFontUtf8Ptr &&
            STATE.nativeFontGlyphsPtr && STATE.nativeFontAtlasPtr &&
            STATE.nativeFontDirtyRectPtr) {
            nativeFontViews.buffer = buf;
            nativeFontViews.input = new Uint8Array(
                buf, STATE.nativeFontInputPtr, STATE.nativeFontInputCapacity
            );
            nativeFontViews.utf8 = new Uint8Array(
                buf, STATE.nativeFontUtf8Ptr, STATE.nativeFontUtf8Capacity
            );
            nativeFontViews.glyphsF32 = new Float32Array(
                buf, STATE.nativeFontGlyphsPtr, STATE.nativeFontGlyphCapacity * 10
            );
            nativeFontViews.glyphsU32 = new Uint32Array(
                buf, STATE.nativeFontGlyphsPtr, STATE.nativeFontGlyphCapacity * 10
            );
            nativeFontViews.atlas = new Uint8Array(
                buf, STATE.nativeFontAtlasPtr, 2048 * 2048
            );
            nativeFontViews.dirtyRect = new Uint32Array(
                buf, STATE.nativeFontDirtyRectPtr, 4
            );
        }
    }
    function getSharedStateViews() {
        ensureValidMemory();
        if (!STATE.sharedStatePtr) throw new Error('[WasmBridge FATAL] sharedStatePtr null');
        return sharedStateViews;
    }
    function getFilterViews() {
        ensureValidMemory();
        if (!STATE.filterArenaPtr) throw new Error('[WasmBridge FATAL] filterArenaPtr null');
        return filterViews;
    }
    function getKawasePassesViews() {
        ensureValidMemory();
        if (!STATE.kawasePassesPtr) throw new Error('[WasmBridge FATAL] kawasePassesPtr null');
        return kawasePassesViews;
    }
    function getTelemetryViews() {
        if (!STATE.ready || !STATE.memory) return null;
        ensureValidMemory();
        if (!STATE.telemetryPtr) return null;
        return telemetryViews;
    }
    function getDisplayListViews() {
        ensureValidMemory();
        if (!STATE.displayListPtr) throw new Error('[WasmBridge FATAL] displayListPtr null');
        return displayListViews;
    }
    function getStylesViews() {
        ensureValidMemory();
        if (!STATE.stylesPtr) throw new Error('[WasmBridge FATAL] stylesPtr null');
        return stylesViews;
    }
    function getTextInputView() {
        ensureValidMemory();
        if (!STATE.textInputBufPtr) throw new Error('[WasmBridge FATAL] textInputBufPtr null');
        return textInputViews.u32;
    }
    function getTextGlyphsViews() {
        ensureValidMemory();
        if (!STATE.textGlyphsPtr) throw new Error('[WasmBridge FATAL] textGlyphsPtr null');
        return textGlyphsViews;
    }
    function getTextResultViews() {
        ensureValidMemory();
        if (!STATE.textResultPtr) throw new Error('[WasmBridge FATAL] textResultPtr null');
        return textResultViews;
    }
    function getGeometryNodesViews() {
        ensureValidMemory();
        if (!STATE.geometryNodesPtr) throw new Error('[WasmBridge FATAL] geometryNodesPtr null');
        return geometryNodesViews;
    }
    function getGeometryResultViews() {
        ensureValidMemory();
        if (!STATE.geometryResultPtr) throw new Error('[WasmBridge FATAL] geometryResultPtr null');
        return geometryResultViews;
    }
    // ========================================================================
    // STOCKFISH BRIDGE INTERFACE
    // ========================================================================
    // Struct Ä‘á»‡m tÄ©nh dÃ¹ng láº¡i 100% (Zero-Allocation theo phong cÃ¡ch Stockfish)
    const _coalesceResult = { count: 0, area: 0 };
    const _projectDurationResult = {
        durationMs: 0,
        hasPercentTimes: false,
        runtimeStatus: 0,
        hasNativeRenderRuntime: false
    };
    const _nativeRenderPlanView = {
        passCount: 0,
        targetCount: 0,
        passesU32: null,
        passesF32: null,
        passesIndex: 0,
        kernelsF32: null,
        kernelsIndex: 0,
        supported: false
    };
    const projectJsonEncoder = new TextEncoder();
    const projectItemsDecoder = new TextDecoder();
    const nativeFontEncoder = new TextEncoder();
    const WasmBridge = {
        init: initWasm,
        whenReady: () => _initPromise || initWasm(),
        isReady: () => STATE.ready,
        STATE: STATE,
        getMode: () => STATE.mode,
        config: {
            _getScratch() {
                const fn = getOptionalExport('wasm_config_get_scratch_ptr');
                return fn ? fn() : 0;
            },
            init() {
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_init');
                if (fn) fn();
            },
            getJson() {
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_get_json');
                if (!fn) return null;
                const bufPtr = this._getScratch();
                if (!bufPtr) return null;
                const written = fn(bufPtr, 4096);
                if (!written) return null;
                return projectItemsDecoder.decode(new Uint8Array(STATE.memory.buffer, bufPtr, written));
            },
            setJson(jsonStr) {
                if (!jsonStr) return false;
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_set_json');
                if (!fn) return false;
                const bufPtr = this._getScratch();
                if (!bufPtr) return false;
                const encoded = projectJsonEncoder.encodeInto(jsonStr, new Uint8Array(STATE.memory.buffer, bufPtr, 4096));
                new Uint8Array(STATE.memory.buffer)[bufPtr + encoded.written] = 0;
                return fn(bufPtr, encoded.written) !== 0;
            },
            getStr(key) {
                if (!key) return '';
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_get_str');
                if (!fn) return '';
                const scratchPtr = this._getScratch();
                if (!scratchPtr) return '';
                const keyPtr = scratchPtr;
                const keyEncoded = projectJsonEncoder.encodeInto(key, new Uint8Array(STATE.memory.buffer, keyPtr, 256));
                new Uint8Array(STATE.memory.buffer)[keyPtr + keyEncoded.written] = 0;
                const outPtr = scratchPtr + 256;
                const written = fn(keyPtr, outPtr, 512);
                if (!written) return '';
                return projectItemsDecoder.decode(new Uint8Array(STATE.memory.buffer, outPtr, written));
            },
            setStr(key, val) {
                if (!key || val === undefined) return false;
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_set_str');
                if (!fn) return false;
                const scratchPtr = this._getScratch();
                if (!scratchPtr) return false;
                const keyPtr = scratchPtr;
                const keyEncoded = projectJsonEncoder.encodeInto(key, new Uint8Array(STATE.memory.buffer, keyPtr, 256));
                new Uint8Array(STATE.memory.buffer)[keyPtr + keyEncoded.written] = 0;
                const valPtr = scratchPtr + 256;
                const valEncoded = projectJsonEncoder.encodeInto(String(val), new Uint8Array(STATE.memory.buffer, valPtr, 512));
                new Uint8Array(STATE.memory.buffer)[valPtr + valEncoded.written] = 0;
                return fn(keyPtr, valPtr) !== 0;
            },
            getDouble(key, defaultVal = 0) {
                if (!key) return defaultVal;
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_get_double');
                if (!fn) return defaultVal;
                const scratchPtr = this._getScratch();
                if (!scratchPtr) return defaultVal;
                const keyPtr = scratchPtr;
                const keyEncoded = projectJsonEncoder.encodeInto(key, new Uint8Array(STATE.memory.buffer, keyPtr, 256));
                new Uint8Array(STATE.memory.buffer)[keyPtr + keyEncoded.written] = 0;
                return fn(keyPtr, defaultVal);
            },
            setDouble(key, val) {
                if (!key || !Number.isFinite(Number(val))) return;
                ensureValidMemory();
                const fn = getOptionalExport('wasm_config_set_double');
                if (!fn) return;
                const scratchPtr = this._getScratch();
                if (!scratchPtr) return;
                const keyPtr = scratchPtr;
                const keyEncoded = projectJsonEncoder.encodeInto(key, new Uint8Array(STATE.memory.buffer, keyPtr, 256));
                new Uint8Array(STATE.memory.buffer)[keyPtr + keyEncoded.written] = 0;
                fn(keyPtr, Number(val));
            }
        },
        doEdit(actionType, jsonText) {
            if (typeof jsonText !== 'string' || !jsonText.length) {
                throw new TypeError('[WasmBridge] Edit payload text is required.');
            }
            ensureValidMemory();
            const bufPtr = STATE.projectJsonPtr;
            const maxLen = STATE.projectJsonCapacity;
            const encoded = projectJsonEncoder.encodeInto(jsonText, new Uint8Array(STATE.memory.buffer, bufPtr, maxLen));
            new Uint8Array(STATE.memory.buffer)[bufPtr + encoded.written] = 0;
            const fn = getOptionalExport('wasm_project_do_edit');
            if (!fn) return false;
            const ok = fn(actionType >>> 0, bufPtr, encoded.written);
            return ok !== 0;
        },
        undo() {
            ensureValidMemory();
            const fn = getOptionalExport('wasm_project_undo');
            return fn ? (fn() !== 0) : false;
        },
        redo() {
            ensureValidMemory();
            const fn = getOptionalExport('wasm_project_redo');
            return fn ? (fn() !== 0) : false;
        },
        canUndo() {
            ensureValidMemory();
            const fn = getOptionalExport('wasm_project_can_undo');
            return fn ? (fn() !== 0) : false;
        },
        canRedo() {
            ensureValidMemory();
            const fn = getOptionalExport('wasm_project_can_redo');
            return fn ? (fn() !== 0) : false;
        },
        getLastDirtyBB() {
            ensureValidMemory();
            const fn = getOptionalExport('wasm_project_get_last_dirty_bb');
            return fn ? fn() : 0n;
        },
        textReveal: {
            flags: {
                get never() {
                    return STATE.textRevealFlagNever;
                },
                get tilde() {
                    return STATE.textRevealFlagTilde;
                }
            },
            getViews() {
                ensureValidMemory();
                if (!STATE.textRevealViews) {
                    throw new Error('[WasmBridge] Native text reveal buffer is unavailable.');
                }
                return STATE.textRevealViews;
            },
            mapOwners(layoutCount, segmentCount, segmentCodepointCount) {
                ensureValidMemory();
                if (!Number.isInteger(layoutCount) || layoutCount < 0 ||
                    layoutCount > STATE.textRevealCapacity ||
                    !Number.isInteger(segmentCount) || segmentCount < 0 ||
                    !STATE.textRevealViews ||
                    segmentCount > STATE.textRevealViews.segmentOffsets.length ||
                    !Number.isInteger(segmentCodepointCount) || segmentCodepointCount < 0 ||
                    segmentCodepointCount > STATE.textRevealCapacity) {
                    throw new RangeError('[WasmBridge] Invalid text reveal owner-map input.');
                }
                return getExport('wasm_text_reveal_map_owners')(
                    layoutCount, segmentCount, segmentCodepointCount
                ) === 1;
            },
            evaluate(count, now) {
                ensureValidMemory();
                if (!Number.isInteger(count) || count < 0 ||
                    count > STATE.textRevealCapacity || !Number.isFinite(now)) {
                    throw new RangeError('[WasmBridge] Invalid native text reveal input.');
                }
                return getExport('wasm_text_reveal_evaluate')(count, now) === 1;
            }
        },
        fontMap: {
            loadManifest(jsonText) {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_load_manifest');
                if (!fn) return false;
                const encoded = new TextEncoder().encode(jsonText);
                const uploadPtr = getExport('wasm_font_map_get_upload_ptr')();
                const uploadCap = getExport('wasm_font_map_get_upload_capacity')();
                if (encoded.length > uploadCap) return false;
                new Uint8Array(STATE.memory.buffer).set(encoded, uploadPtr);
                return fn(uploadPtr, encoded.length) === 1;
            },
            registerFile(path, bytes) {
                ensureValidMemory();
                const commitFn = getExport('wasm_font_map_commit_file');
                const uploadPtrFn = getExport('wasm_font_map_get_upload_ptr');
                const uploadCapFn = getExport('wasm_font_map_get_upload_capacity');
                if (!commitFn || !uploadPtrFn || !uploadCapFn) return 0;
                const uploadCap = uploadCapFn();
                if (!(bytes instanceof Uint8Array) || bytes.length === 0 || bytes.length > uploadCap) return 0;
                const uploadPtr = uploadPtrFn();
                new Uint8Array(STATE.memory.buffer).set(bytes, uploadPtr);
                const pathBytes = new TextEncoder().encode(path);
                const pathPtr = uploadPtr + ((bytes.length + 15) & ~15);
                if (pathPtr + pathBytes.length + 1 > uploadPtr + uploadCap) return 0;
                new Uint8Array(STATE.memory.buffer).set(pathBytes, pathPtr);
                new Uint8Array(STATE.memory.buffer)[pathPtr + pathBytes.length] = 0;
                const fileId = commitFn(pathPtr, pathBytes.length, bytes.length);
                if (fileId) {
                    const invalidateTextCaches = getExport('wasm_renderer_invalidate_text_caches');
                    if (invalidateTextCaches) invalidateTextCaches();
                    if (window.CanvasCompositor && typeof window.CanvasCompositor.invalidateScene === 'function') {
                        window.CanvasCompositor.invalidateScene('FONT_LOADED');
                    }
                    if (typeof window.updateFrame === 'function') {
                        window.updateFrame();
                    }
                }
                return fileId;
            },
            resolveFile(family, weight = 400, isItalic = false) {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_resolve_file');
                if (!fn) return 0;
                const famBytes = new TextEncoder().encode(family || '');
                const uploadPtr = getExport('wasm_font_map_get_upload_ptr')();
                new Uint8Array(STATE.memory.buffer).set(famBytes, uploadPtr);
                new Uint8Array(STATE.memory.buffer)[uploadPtr + famBytes.length] = 0;
                return fn(uploadPtr, famBytes.length, Number(weight) || 400, isItalic ? 1 : 0);
            },
            lookup(family, weight = 400, isItalic = false) {
                ensureValidMemory();
                const fileId = this.resolveFile(family, weight, isItalic);
                if (!fileId) return null;
                const uploadPtr = getExport('wasm_font_map_get_upload_ptr')();
                const getPathFn = getExport('wasm_font_map_get_file_path');
                const pathLen = getPathFn ? getPathFn(fileId, uploadPtr, 128) : 0;
                let pathStr = '';
                if (pathLen > 0) {
                    pathStr = new TextDecoder().decode(new Uint8Array(STATE.memory.buffer).subarray(uploadPtr, uploadPtr + pathLen));
                }
                const bufPtr = getExport('wasm_font_map_get_file_buffer_ptr')(fileId);
                const bufSize = getExport('wasm_font_map_get_file_buffer_size')(fileId);
                const fontKey = getExport('wasm_font_map_get_file_key')(fileId);
                return {
                    fileId,
                    path: pathStr,
                    bufferPtr: bufPtr,
                    bufferSize: bufSize,
                    fontKey,
                    buffer: (bufPtr && bufSize > 0) ? new Uint8Array(STATE.memory.buffer).subarray(bufPtr, bufPtr + bufSize) : null
                };
            },
            loadFontByFileId(fileId) {
                ensureValidMemory();
                const fn = getExport('wasm_text_font_load_by_file_id');
                return fn ? fn(fileId >>> 0) : 0;
            },
            getFamilyCount() {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_get_family_count');
                return fn ? fn() : 0;
            },
            getFileCount() {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_get_file_count');
                return fn ? fn() : 0;
            },
            loadManifestFile(path = 'Asset/font/fonts.json') {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_load_manifest_file');
                if (!fn) return false;
                const pathBytes = new TextEncoder().encode(path);
                const uploadPtr = getExport('wasm_font_map_get_upload_ptr')();
                new Uint8Array(STATE.memory.buffer).set(pathBytes, uploadPtr);
                new Uint8Array(STATE.memory.buffer)[uploadPtr + pathBytes.length] = 0;
                return fn(uploadPtr) === 1;
            },
            loadFontFileDirect(path) {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_load_file_direct');
                if (!fn || !path) return 0;
                const pathBytes = new TextEncoder().encode(path);
                const uploadPtr = getExport('wasm_font_map_get_upload_ptr')();
                new Uint8Array(STATE.memory.buffer).set(pathBytes, uploadPtr);
                new Uint8Array(STATE.memory.buffer)[uploadPtr + pathBytes.length] = 0;
                return fn(uploadPtr);
            },
            loadAllFonts() {
                ensureValidMemory();
                const fn = getExport('wasm_font_map_load_all_fonts');
                return fn ? fn() : 0;
            }
        },
        nativeFont: {
            load(fontBytes) {
                ensureValidMemory();
                if (!(fontBytes instanceof Uint8Array) || fontBytes.byteLength === 0) {
                    throw new TypeError('[WasmBridge] Native font input must be non-empty Uint8Array data.');
                }
                if (fontBytes.byteLength > STATE.nativeFontInputCapacity) {
                    throw new RangeError(
                        `[WasmBridge] Font exceeds native buffer capacity (${STATE.nativeFontInputCapacity} bytes).`
                    );
                }
                let key = 2166136261;
                for (let i = 0; i < fontBytes.length; i++) {
                    key = Math.imul(key ^ fontBytes[i], 16777619) >>> 0;
                }
                if (key === 0) key = 1;
                nativeFontViews.input.set(fontBytes);
                const loaded = getExport('wasm_text_font_load')(
                    key, fontBytes.byteLength
                );
                if (loaded !== 1) {
                    const status = getExport('wasm_text_font_get_status')();
                    throw new Error(`[WasmBridge] Native font loading failed (status ${status}).`);
                }
                STATE.nativeFontKey = key;
                return key;
            },
            shape(text, pixelSize) {
                ensureValidMemory();
                if (!STATE.nativeFontKey) {
                    throw new Error('[WasmBridge] Load a native font before shaping text.');
                }
                if (typeof text !== 'string' || !Number.isFinite(pixelSize) || pixelSize <= 0) {
                    throw new TypeError('[WasmBridge] Invalid native text shaping input.');
                }
                const encoded = nativeFontEncoder.encodeInto(text, nativeFontViews.utf8);
                if (encoded.read !== text.length) {
                    throw new RangeError(
                        `[WasmBridge] Text exceeds native UTF-8 buffer capacity (${STATE.nativeFontUtf8Capacity} bytes).`
                    );
                }
                const count = getExport('wasm_text_font_shape')(encoded.written, pixelSize);
                if (count === 0 && text.length !== 0) {
                    const status = getExport('wasm_text_font_get_status')();
                    throw new Error(`[WasmBridge] Native text shaping failed (status ${status}).`);
                }
                return { count, views: nativeFontViews };
            },
            resetAtlasDirtyRect() {
                ensureValidMemory();
                getExport('wasm_text_font_reset_atlas_dirty_rect')();
            },
            getBaselineOffset() {
                ensureValidMemory();
                const fn = getExport('wasm_text_font_get_baseline_offset');
                return fn ? fn() : 0;
            },
            getViews() {
                ensureValidMemory();
                return nativeFontViews;
            }
        },
        manager: {
            _executeNativeLoad(written, stageW = 1920, stageH = 1080) {
                const maxStageDimension = 3.402823466e38;
                const requestedStageW = Number(stageW);
                const requestedStageH = Number(stageH);
                const normalizedStageW = Number.isFinite(requestedStageW) &&
                    requestedStageW > 0 && requestedStageW <= maxStageDimension
                    ? requestedStageW
                    : 1920;
                const normalizedStageH = Number.isFinite(requestedStageH) &&
                    requestedStageH > 0 && requestedStageH <= maxStageDimension
                    ? requestedStageH
                    : 1080;
                getExport('wasm_manager_load_project_json')(
                    written,
                    normalizedStageW,
                    normalizedStageH
                );
                ensureValidMemory();
                const compileStatus =
                    getExport('wasm_manager_get_project_compile_status')();
                if (compileStatus !== 0) {
                    const compileStatusNames = {
                        1: 'invalid JSON',
                        2: 'missing timeline array',
                        3: 'invalid timeline',
                        4: 'scene capacity exceeded',
                        5: 'compiled output capacity exceeded',
                        6: 'template capacity exceeded',
                        7: 'field capacity exceeded',
                        8: 'invalid timeline time',
                        9: 'project JSON input capacity exceeded'
                    };
                    const reason = compileStatusNames[compileStatus] ||
                        `unknown native status ${compileStatus}`;
                    throw new Error(
                        `[WasmBridge] Native project compiler rejected project JSON: ${reason}.`
                    );
                }
                _projectDurationResult.runtimeStatus =
                    getExport('wasm_timeline_runtime_get_load_status')();
                _projectDurationResult.hasNativeRenderRuntime =
                    _projectDurationResult.runtimeStatus === 0;
                if (_projectDurationResult.runtimeStatus !== 0 &&
                    typeof console !== 'undefined' && console.warn) {
                    console.warn(
                        '[WasmBridge] Native timeline render runtime rejected this project; unsupported timeline effects have no JavaScript fallback.',
                        _projectDurationResult.runtimeStatus
                    );
                }
                const resultPtr = STATE.projectDurationResultPtr;
                const resultF64Index = resultPtr >> 3;
                const resultU32Index = resultPtr >> 2;
                const durationMs = STATE.views.f64[resultF64Index];
                if (!Number.isFinite(durationMs) || durationMs < 0) {
                    throw new Error(
                        `[WasmBridge] Native project manager failed to load project (status ${_projectDurationResult.runtimeStatus}).`
                    );
                }
                _projectDurationResult.durationMs = durationMs;
                _projectDurationResult.hasPercentTimes =
                    (STATE.views.u32[resultU32Index + 2] & 1) !== 0;
                const compiledLength =
                    getExport('wasm_manager_get_compiled_items_json_length')();
                if (!compiledLength || STATE.compiledItemsJsonPtr + compiledLength >
                    STATE.views.u8.length) {
                    throw new Error('[WasmBridge] Native project compiler returned an invalid item buffer.');
                }
                _projectDurationResult.items = JSON.parse(
                    projectItemsDecoder.decode(STATE.views.u8.subarray(
                        STATE.compiledItemsJsonPtr,
                        STATE.compiledItemsJsonPtr + compiledLength
                    ))
                );
                return _projectDurationResult;
            },
            // [BYPASS 1]: Zero-Copy direct buffer mapping from FileSystem into Wasm memory
            loadProjectBuffer(buffer, stageW = 1920, stageH = 1080) {
                ensureValidMemory();
                if (!STATE.ready || !STATE.projectJsonView) {
                    throw new Error('[WasmBridge] Native project manager is not ready.');
                }
                const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
                if (bytes.byteLength === 0) {
                    throw new Error('[WasmBridge] Project buffer is empty.');
                }
                if (bytes.byteLength > STATE.projectJsonCapacity) {
                    throw new RangeError(
                        `[WasmBridge] Project JSON exceeds native shared-buffer capacity (${STATE.projectJsonCapacity} bytes).`
                    );
                }
                STATE.projectJsonView.set(bytes);
                return this._executeNativeLoad(bytes.byteLength, stageW, stageH);
            },
            loadProjectJson(jsonText, stageW = 1920, stageH = 1080) {
                if (typeof jsonText !== 'string') {
                    throw new TypeError('[WasmBridge] Project manager input must be JSON text.');
                }
                ensureValidMemory();
                if (!STATE.ready || !STATE.projectJsonView) {
                    throw new Error('[WasmBridge] Native project manager is not ready.');
                }
                const encoded = projectJsonEncoder.encodeInto(jsonText, STATE.projectJsonView);
                if (encoded.read !== jsonText.length || encoded.written === 0) {
                    throw new RangeError(
                        `[WasmBridge] Project JSON exceeds native shared-buffer capacity (${STATE.projectJsonCapacity} bytes).`
                    );
                }
                return this._executeNativeLoad(encoded.written, stageW, stageH);
            },
            readNnueCutTelemetry() {
                ensureValidMemory();
                const scoreFn = getOptionalExport('wasm_nnue_cut_get_score');
                const artFn = getOptionalExport('wasm_nnue_cut_get_artistic_score');
                const plyFn = getOptionalExport('wasm_nnue_cut_get_edit_ply');
                const mutFn = getOptionalExport('wasm_nnue_cut_get_mutation_count');
                const ptrFn = getOptionalExport('wasm_nnue_cut_get_report_json_ptr');
                const lenFn = getOptionalExport('wasm_nnue_cut_get_report_json_length');
                let report = null;
                if (ptrFn && lenFn) {
                    const ptr = ptrFn() >>> 0;
                    const len = lenFn() >>> 0;
                    if (ptr && len && ptr + len <= STATE.views.u8.length) {
                        try {
                            report = JSON.parse(projectItemsDecoder.decode(STATE.views.u8.subarray(ptr, ptr + len)));
                        } catch (_) {}
                    }
                }
                return {
                    score: scoreFn ? scoreFn() : 0,
                    artisticScore: artFn ? artFn() : 0,
                    editPly: plyFn ? (plyFn() >>> 0) : 0,
                    mutationCount: mutFn ? (mutFn() >>> 0) : 0,
                    report
                };
            },
            doJsonMove(jsonText, stageW = 1920, stageH = 1080) {
                if (typeof jsonText !== 'string' || !jsonText.length) {
                    throw new TypeError('[WasmBridge] Project JSON text is required.');
                }
                ensureValidMemory();
                const encoded = projectJsonEncoder.encodeInto(jsonText, STATE.projectJsonView);
                if (encoded.read !== jsonText.length || encoded.written === 0) {
                    throw new RangeError(`[WasmBridge] Project JSON exceeds native capacity (${STATE.projectJsonCapacity} bytes).`);
                }
                const normalizedStageW = (Number.isFinite(Number(stageW)) && Number(stageW) > 0) ? Number(stageW) : 1920;
                const normalizedStageH = (Number.isFinite(Number(stageH)) && Number(stageH) > 0) ? Number(stageH) : 1080;
                const moveFn = getOptionalExport('wasm_manager_do_json_move') || getExport('wasm_manager_load_project_json');
                moveFn(encoded.written, normalizedStageW, normalizedStageH);
                ensureValidMemory();
                const compileStatus = getExport('wasm_manager_get_project_compile_status')();
                if (compileStatus !== 0) {
                    throw new Error(`[WasmBridge] Native project compiler rejected JSON move (status ${compileStatus}).`);
                }
                const resultPtr = STATE.projectDurationResultPtr;
                const resultF64Index = resultPtr >> 3;
                const resultU32Index = resultPtr >> 2;
                const durationMs = STATE.views.f64[resultF64Index];
                _projectDurationResult.durationMs = durationMs;
                _projectDurationResult.hasPercentTimes = (STATE.views.u32[resultU32Index + 2] & 1) !== 0;
                const compiledLength = getExport('wasm_manager_get_compiled_items_json_length')();
                if (compiledLength && STATE.compiledItemsJsonPtr + compiledLength <= STATE.views.u8.length) {
                    _projectDurationResult.items = JSON.parse(
                        projectItemsDecoder.decode(STATE.views.u8.subarray(
                            STATE.compiledItemsJsonPtr,
                            STATE.compiledItemsJsonPtr + compiledLength
                        ))
                    );
                }
                _projectDurationResult.nnueCut = this.readNnueCutTelemetry();
                return _projectDurationResult;
            },
            setNativeBassEnvelope(slot, bass) {
                ensureValidMemory();
                if (!bass || !bass.level || !bass.hit ||
                    bass.level.length !== bass.hit.length ||
                    !Number.isFinite(Number(bass.hop)) || bass.hop <= 0) {
                    throw new Error('[WasmBridge] Invalid native bass envelope.');
                }
                const count = bass.level.length >>> 0;
                const setChunk = getExport('wasm_timeline_runtime_set_bass_envelope');
                if (!STATE.textInputBufPtr) {
                    throw new Error('[WasmBridge] Native bass-envelope input buffer is unavailable.');
                }
                const input = new Float32Array(
                    STATE.memory.buffer, STATE.textInputBufPtr, 4096);
                const samplesPerChunk = input.length >>> 1;
                for (let offset = 0; offset < count; offset += samplesPerChunk) {
                    const length = Math.min(samplesPerChunk, count - offset);
                    for (let index = 0; index < length; index++) {
                        input[index * 2] = bass.level[offset + index];
                        input[index * 2 + 1] = bass.hit[offset + index];
                    }
                    if (!setChunk(
                        slot >>> 0, Number(bass.hop), count, offset, length,
                        STATE.textInputBufPtr
                    )) {
                        throw new Error(
                            '[WasmBridge] Native bass-envelope capacity or slot validation failed.');
                    }
                    ensureValidMemory();
                }
            },
            analyzeAndBakeAudio(pcmSamples, sampleRate, cutoff, sensitivity, threshold, release, durationMs) {
                ensureValidMemory();
                if (!pcmSamples || !pcmSamples.length || !(sampleRate > 0)) return false;
                const allocFn = getExport('wasm_audio_alloc_pcm');
                const freeFn = getExport('wasm_audio_free_pcm');
                const analyzeFn = getExport('wasm_audio_analyze_and_bake');
                if (!allocFn || !freeFn || !analyzeFn) return false;
                const sampleCount = pcmSamples.length >>> 0;
                const pcmPtr = allocFn(sampleCount);
                if (!pcmPtr) return false;
                ensureValidMemory();
                const wasmF32 = new Float32Array(STATE.memory.buffer, pcmPtr, sampleCount);
                wasmF32.set(pcmSamples);
                const framesBaked = analyzeFn(
                    pcmPtr,
                    sampleCount,
                    sampleRate >>> 0,
                    Number(cutoff) || 150,
                    Number(sensitivity) || 1.0,
                    Number(threshold) || 0.2,
                    Number(release) || 180,
                    Number(durationMs) || 0
                );
                freeFn(pcmPtr);
                ensureValidMemory();
                return framesBaked > 0;
            },
            getMusicScriptEnvelope() {
                ensureValidMemory();
                const getLevelsPtr = getExport('wasm_music_script_get_levels_ptr');
                const getHitsPtr = getExport('wasm_music_script_get_hits_ptr');
                const getCap = getExport('wasm_music_script_get_capacity');
                if (!getLevelsPtr || !getHitsPtr || !getCap) return null;
                const levelsPtr = getLevelsPtr();
                const hitsPtr = getHitsPtr();
                const cap = getCap() >>> 0;
                if (!levelsPtr || !hitsPtr || !cap) return null;
                return {
                    hop: 0.010,
                    level: new Float32Array(STATE.memory.buffer, levelsPtr, cap),
                    hit: new Uint16Array(STATE.memory.buffer, hitsPtr, cap)
                };
            },
            bakeMusicScript(bass, durationMs) {
                ensureValidMemory();
                if (!bass || !bass.level || !bass.hit ||
                    !Number.isFinite(Number(bass.hop)) || bass.hop <= 0) {
                    return false;
                }
                const count = Math.min(bass.level.length, bass.hit.length) >>> 0;
                const getLevelsPtr = getExport('wasm_music_script_get_levels_ptr');
                const getHitsPtr = getExport('wasm_music_script_get_hits_ptr');
                const getCap = getExport('wasm_music_script_get_capacity');
                const bakeInplace = getExport('wasm_music_script_bake_inplace');
                const bakeFn = getExport('wasm_music_script_bake');
                if (getLevelsPtr && getHitsPtr && bakeInplace && getCap) {
                    const levelsPtr = getLevelsPtr();
                    const hitsPtr = getHitsPtr();
                    const capacity = getCap() >>> 0;
                    if (levelsPtr && hitsPtr && capacity > 0) {
                        const length = Math.min(count, capacity);
                        const f32 = new Float32Array(STATE.memory.buffer, levelsPtr, length);
                        const u16 = new Uint16Array(STATE.memory.buffer, hitsPtr, length);
                        for (let i = 0; i < length; i++) {
                            f32[i] = bass.level[i];
                            u16[i] = bass.hit[i] & 0xFFFF;
                        }
                        return bakeInplace(length, Number(bass.hop), Number(durationMs) || 0) === 1;
                    }
                }
                if (!bakeFn || !STATE.textInputBufPtr) return false;
                const maxChunk = 2048;
                const inputF32 = new Float32Array(STATE.memory.buffer, STATE.textInputBufPtr, maxChunk);
                const inputU16 = new Uint16Array(STATE.memory.buffer, STATE.textInputBufPtr + maxChunk * 4, maxChunk);
                const length = Math.min(count, maxChunk);
                for (let i = 0; i < length; i++) {
                    inputF32[i] = bass.level[i];
                    inputU16[i] = bass.hit[i] & 0xFFFF;
                }
                return bakeFn(
                    STATE.textInputBufPtr,
                    STATE.textInputBufPtr + maxChunk * 4,
                    length,
                    Number(bass.hop),
                    Number(durationMs) || 0
                ) === 1;
            },
            renderNativeRenderPlanAt(timeMs, width, height) {
                ensureValidMemory();
                if (!_projectDurationResult.hasNativeRenderRuntime) return null;
                if (!getExport('wasm_timeline_runtime_render_at')(
                    Number(timeMs), width >>> 0, height >>> 0
                )) {
                    throw new Error('[WasmBridge] Native timeline render-plan evaluation failed.');
                }
                ensureValidMemory();
                const passCount = getExport('wasm_timeline_runtime_get_pass_count')();
                const targetCount =
                    getExport('wasm_timeline_runtime_get_target_count')();
                const passesPtr = getExport('wasm_timeline_runtime_get_passes')();
                const kernelsPtr = getExport('wasm_timeline_runtime_get_kernels')();
                if (!passesPtr || !kernelsPtr ||
                    (passesPtr & 3) !== 0 || (kernelsPtr & 3) !== 0) {
                    throw new Error('[WasmBridge] Native render-plan buffers are invalid.');
                }
                _nativeRenderPlanView.passCount = passCount;
                _nativeRenderPlanView.targetCount = targetCount;
                _nativeRenderPlanView.passesU32 = STATE.views.u32;
                _nativeRenderPlanView.passesF32 = STATE.views.f32;
                _nativeRenderPlanView.passesIndex = passesPtr >> 2;
                _nativeRenderPlanView.kernelsF32 = STATE.views.f32;
                _nativeRenderPlanView.kernelsIndex = kernelsPtr >> 2;
                _nativeRenderPlanView.supported =
                    getExport('wasm_timeline_runtime_is_supported_render_plan')() !== 0;
                return _nativeRenderPlanView;
            },
            parseTimelineColor(color, fallback = 0xFFFFFFFF) {
                ensureValidMemory();
                const defaultColor = Number(fallback) >>> 0;
                if (!color) return defaultColor;
                if (!STATE.timelineSetupInputView) {
                    throw new Error('[WasmBridge] Native timeline color input buffer is unavailable.');
                }
                const text = String(color);
                const length = writeTimelineSetupText(text, 0);
                return getExport('wasm_manager_parse_timeline_color')(
                    length, defaultColor
                ) >>> 0;
            },
            resolveTimelineTime(value, reference, isKeyframeOffset = false) {
                ensureValidMemory();
                if (!isKeyframeOffset &&
                    (value === undefined || value === null || value === '')) {
                    return null;
                }
                if (!STATE.timelineSetupInputView || !STATE.timelineTimeResultView) {
                    throw new Error('[WasmBridge] Native timeline time buffers are unavailable.');
                }
                const isString = typeof value === 'string';
                const length = isString ? writeTimelineSetupText(value, 0) : 0;
                const numericValue = isString ? 0 : Number(value);
                const result = getExport('wasm_manager_resolve_timeline_time')(
                    length,
                    numericValue,
                    Number(reference),
                    isKeyframeOffset ? 1 : 0
                );
                if (!result) {
                    throw new Error('[WasmBridge] Native timeline time conversion rejected input.');
                }
                const values = STATE.timelineTimeResultView;
                if (isKeyframeOffset) {
                    return { ms: values[0], offset: values[1] };
                }
                if (!values[1]) return null;
                return values[0];
            },
            classifyTimelineItemTier(type, flags, keyframeCount, hasReveal) {
                const mediaType = type === 'audio' ? 7 :
                    type === 'effect' ? 8 :
                        type === 'video' ? 2 : 0;
                const fn = getExport('wasm_manager_classify_timeline_item_tier');
                if (!fn) {
                    throw new Error('[WasmBridge] Native timeline tier classifier is unavailable.');
                }
                return fn(
                    mediaType,
                    flags >>> 0,
                    keyframeCount >>> 0,
                    hasReveal ? 1 : 0
                );
            },
            selectCompositeFrameMode(sceneValid, dirtyArea, fullArea, dirtyCount) {
                return getExport('wasm_manager_select_composite_frame_mode')(
                    sceneValid ? 1 : 0,
                    dirtyArea,
                    fullArea,
                    dirtyCount >>> 0
                );
            },
            selectStaticCompositePrefix(count) {
                ensureValidMemory();
                if (!Number.isInteger(count) || count < 0 || count > 256) {
                    throw new RangeError('[WasmBridge] Invalid composite command count.');
                }
                return getExport('wasm_manager_select_static_composite_prefix')(count);
            },
            getCompositeRenderCulledCount() {
                return getExport('wasm_manager_get_composite_render_culled_count')();
            },
            orderCompositeEffects(effects) {
                ensureValidMemory();
                if (!Array.isArray(effects)) {
                    throw new TypeError('[WasmBridge] Composite effects must be an array.');
                }
                if (!STATE.compositeEffectOrderInputFloatView ||
                    !STATE.compositeEffectOrderInputUintView ||
                    !STATE.compositeEffectOrderOutputView) {
                    throw new Error('[WasmBridge] Native composite effect-order buffers are unavailable.');
                }
                let count = 0;
                const inputFloat = STATE.compositeEffectOrderInputFloatView;
                const inputUint = STATE.compositeEffectOrderInputUintView;
                for (let i = 0; i < effects.length; i++) {
                    const effect = effects[i];
                    if (!effect || effect.type !== 'effect') continue;
                    if (count >= 64) {
                        throw new RangeError('[WasmBridge] Composite effect count exceeds native capacity.');
                    }
                    const floatBase = count * 3;
                    const uintBase = count * 6;
                    inputFloat[floatBase] = Number(effect.z) || 0;
                    inputUint[uintBase + 2] = (Number(effect.seq) || 0) >>> 0;
                    inputUint[uintBase + 3] = i;
                    inputUint[uintBase + 4] =
                        effect.effect === 'intro' || effect.effect === 'outro' ? 1 : 0;
                    count++;
                }
                return getExport('wasm_manager_order_composite_effects')(count);
            },
            getCompositeEffectOrderView() {
                ensureValidMemory();
                if (!STATE.compositeEffectOrderOutputView) {
                    throw new Error('[WasmBridge] Native composite effect-order output is unavailable.');
                }
                return STATE.compositeEffectOrderOutputView;
            },
            computeCompositeStaticKey(count, red, green, blue, alpha) {
                ensureValidMemory();
                if (!Number.isInteger(count) || count < 0 || count > 256) {
                    throw new RangeError('[WasmBridge] Invalid static-composite command count.');
                }
                return getExport('wasm_manager_compute_composite_static_key')(
                    count, red, green, blue, alpha
                );
            },
            parseCompositeCssColor(color, opacity, out) {
                ensureValidMemory();
                if (typeof color !== 'string' || color.length > 256 ||
                    !Number.isFinite(opacity) || !STATE.compositeCssColorBufferView ||
                    !STATE.compositeCssColorResultView) {
                    throw new RangeError('[WasmBridge] Invalid CSS color input.');
                }
                const input = STATE.compositeCssColorBufferView;
                for (let i = 0; i < color.length; i++) {
                    const code = color.charCodeAt(i);
                    if (code > 0x7F) {
                        throw new RangeError('[WasmBridge] CSS color must contain ASCII characters.');
                    }
                    input[i] = code;
                }
                getExport('wasm_manager_parse_composite_css_color')(color.length, opacity);
                out[0] = STATE.compositeCssColorResultView[0];
                out[1] = STATE.compositeCssColorResultView[1];
                out[2] = STATE.compositeCssColorResultView[2];
                out[3] = STATE.compositeCssColorResultView[3];
                return out;
            },
            computeTimelineGeometry(input) {
                ensureValidMemory();
                if (!STATE.timelineSetupInputView || !STATE.timelineGeometryResultView ||
                    !input || typeof input !== 'object') {
                    throw new Error('[WasmBridge] Native timeline geometry buffers are unavailable.');
                }
                let offset = 0;
                const widthLength = writeTimelineSetupText(input.width, offset);
                offset += widthLength;
                const heightLength = writeTimelineSetupText(input.height, offset);
                offset += heightLength;
                const leftLength = writeTimelineSetupText(input.left, offset);
                offset += leftLength;
                const topLength = writeTimelineSetupText(input.top, offset);
                offset += topLength;
                const transformLength = writeTimelineSetupText(input.transform, offset);
                offset += transformLength;
                const translateLength = writeTimelineSetupText(input.translate, offset);
                const resultPtr = getExport('wasm_manager_compute_timeline_geometry')(
                    input.flags >>> 0,
                    Number(input.stageW),
                    Number(input.stageH),
                    Number(input.intrinsicW || input.stageW),
                    Number(input.intrinsicH || input.stageH),
                    widthLength,
                    heightLength,
                    leftLength,
                    topLength,
                    transformLength,
                    translateLength
                );
                if (!resultPtr) {
                    throw new Error('[WasmBridge] Native timeline geometry rejected CSS input.');
                }
                return STATE.timelineGeometryResultView;
            },
            computeTimelineKeyframeGeometry(input) {
                ensureValidMemory();
                if (!STATE.timelineSetupInputView || !STATE.timelineGeometryResultView ||
                    !input || typeof input !== 'object') {
                    throw new Error('[WasmBridge] Native timeline geometry buffers are unavailable.');
                }
                let offset = 0;
                const widthLength = writeTimelineSetupText(input.width, offset);
                offset += widthLength;
                const heightLength = writeTimelineSetupText(input.height, offset);
                offset += heightLength;
                const leftLength = writeTimelineSetupText(input.left, offset);
                offset += leftLength;
                const topLength = writeTimelineSetupText(input.top, offset);
                const resultPtr = getExport('wasm_manager_compute_timeline_keyframe_geometry')(
                    input.flags >>> 0,
                    Number(input.stageW),
                    Number(input.stageH),
                    Number(input.anchorX),
                    Number(input.anchorY),
                    Number(input.baseX),
                    Number(input.baseY),
                    Number(input.baseW),
                    Number(input.baseH),
                    widthLength,
                    heightLength,
                    leftLength,
                    topLength
                );
                if (!resultPtr) {
                    throw new Error('[WasmBridge] Native keyframe geometry rejected CSS input.');
                }
                return STATE.timelineGeometryResultView;
            },
            normalizeTimelineTextStyle(input) {
                ensureValidMemory();
                if (!STATE.timelineSetupInputView || !STATE.timelineTextStyleResultView ||
                    !input || typeof input !== 'object') {
                    throw new Error('[WasmBridge] Native text-style buffers are unavailable.');
                }
                let offset = 0;
                const fontLength = writeTimelineSetupText(input.fontFamily, offset);
                offset += fontLength;
                const weightLength = writeTimelineSetupText(input.fontWeight, offset);
                offset += weightLength;
                const alignLength = writeTimelineSetupText(input.textAlign, offset);
                offset += alignLength;
                const revealLength = writeTimelineSetupText(input.revealMode, offset);
                offset += revealLength;
                const transformLength = writeTimelineSetupText(input.textTransform, offset);
                offset += transformLength;
                const easingLength = writeTimelineSetupText(input.easing, offset);
                const result = getExport('wasm_manager_normalize_timeline_text_style')(
                    fontLength,
                    weightLength,
                    alignLength,
                    revealLength,
                    transformLength,
                    easingLength
                );
                if (!result) {
                    throw new Error('[WasmBridge] Native text-style normalization rejected input.');
                }
                return STATE.timelineTextStyleResultView;
            },
            getProjectGeneration() {
                return getExport('wasm_manager_get_project_generation')();
            },
            renderAt(
                W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
                lastFrameHash, occludedMask, nowMs, frameIndex, isPlaying
            ) {
                ensureValidMemory();
                const resPtr = getExport('wasm_manager_render_at')(
                    W >>> 0, H >>> 0, styleEpoch >>> 0, fontsVer >>> 0,
                    styleVer >>> 0, mediaVer >>> 0, bgHash >>> 0,
                    BigInt(lastFrameHash || 0), BigInt(occludedMask || 0),
                    Number(nowMs) || 0, frameIndex >>> 0, isPlaying ? 1 : 0
                );
                if (!resPtr) {
                    throw new Error('[WasmBridge] Native renderAt requires a loaded project.');
                }
                const u32Idx = resPtr >> 2;
                const u64Idx = resPtr >> 3;
                const resHash = STATE.views.u64
                    ? STATE.views.u64[u64Idx]
                    : (BigInt(STATE.views.u32[u32Idx]) |
                        (BigInt(STATE.views.u32[u32Idx + 1]) << 32n));
                const activeMask = STATE.views.u64
                    ? STATE.views.u64[u64Idx + 3]
                    : (BigInt(STATE.views.u32[u32Idx + 6]) |
                        (BigInt(STATE.views.u32[u32Idx + 7]) << 32n));
                return {
                    currentHash: resHash,
                    decision: STATE.views.u32[u32Idx + 2],
                    ttData: STATE.views.u32[u32Idx + 3],
                    ttTier: STATE.views.u32[u32Idx + 4],
                    culledCount: STATE.views.u32[u32Idx + 5],
                    activeMask
                };
            }
        },
        getTierEnum() {
            const v = getExport('wasm_get_tier_enum')();
            return [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF];
        },
        // --------------------------------------------------------------------
        // 1. Gá»˜P VÃ™NG Báº¨N (DIRTY RECTANGLE COALESCENCE)
        // --------------------------------------------------------------------
        coalesceDirty(dirtyBuf, dirtyN, W, H, maxRects = 8) {
            ensureValidMemory();
            const f64Offset = STATE.dirtyBufPtr >> 3;
            const len = dirtyN << 2;
            STATE.views.f64.set(dirtyBuf.subarray(0, len), f64Offset);
            const fn = getExport('wasm_coalesce_dirty');
            const newN = fn(dirtyN, W, H);
            const areaIdx = STATE.areaPtr >> 3;
            const totalArea = STATE.views.f64[areaIdx];
            dirtyBuf.set(STATE.views.f64.subarray(f64Offset, f64Offset + (newN << 2)));
            _coalesceResult.count = newN;
            _coalesceResult.area = totalArea;
            return _coalesceResult;
        },
        // --------------------------------------------------------------------
        // 2. BÄ‚M KHUNG HÃŒNH (ZOBRIST 64-BIT FRAME HASH)
        // --------------------------------------------------------------------
        computeZobrist(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, itemKeysBuf, count) {
            ensureValidMemory();
            const u64Offset = STATE.itemKeysBufPtr >> 3;
            const n = Math.min(count, STATE.itemKeysCapacity);
            STATE.views.u64.set(itemKeysBuf.subarray(0, n), u64Offset);
            const fn = getExport('wasm_compute_zobrist');
            const rawHash = fn(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, n);
            return BigInt.asUintN(64, rawHash);
        },
        // --------------------------------------------------------------------
        // 3. FFT PHá»” BIáº¾N Äá»”I (FAST REAL FFT 2048)
        // --------------------------------------------------------------------
        fftTransform(samples, offset, totalSamples, outMag) {
            ensureValidMemory();
            const validCount = Math.max(0, Math.min(2048, totalSamples - offset));
            const sampleF32Idx = STATE.sampleBufPtr >> 2;
            if (validCount > 0) {
                STATE.views.f32.set(samples.subarray(offset, offset + validCount), sampleF32Idx);
            }
            if (validCount < 2048) {
                STATE.views.f32.fill(0, sampleF32Idx + validCount, sampleF32Idx + 2048);
            }
            const fn = getExport('wasm_fft_transform_frame');
            fn(validCount);
            const magF32Idx = STATE.magBufPtr >> 2;
            outMag.set(STATE.views.f32.subarray(magF32Idx, magF32Idx + 1025));
            return true;
        },
        // --------------------------------------------------------------------
        // 4. Báº¢NG HOÃN Vá»Š (TRANSPOSITION TABLE - STOCKFISH CLUSTER MODEL)
        // --------------------------------------------------------------------
        ttProbe(key) {
            ensureValidMemory();
            const fn = getExport('wasm_tt_probe');
            const hit = fn(BigInt(key), STATE.areaPtr, STATE.areaPtr + 4);
            if (hit) {
                const u32Idx = STATE.areaPtr >> 2;
                return {
                    hit: true,
                    data: STATE.views.u32[u32Idx],
                    tier: STATE.views.u32[u32Idx + 1]
                };
            }
            return { hit: false, data: 0, tier: 0 };
        },
        ttWrite(key, data, tier = 0) {
            const fn = getExport('wasm_tt_write');
            fn(BigInt(key), data >>> 0, tier >>> 0);
            return true;
        },
        ttClear() {
            const fn = getExport('wasm_tt_clear');
            fn();
            return true;
        },
        ttNextGen() {
            const fn = getExport('wasm_tt_next_gen');
            fn();
            return true;
        },
        ttSaveDisplayList(key, tier = 0) {
            const fn = getExport('wasm_tt_save_display_list');
            return fn(BigInt(key), tier >>> 0);
        },
        ttRestoreDisplayList(slot, expectedKey = 0n) {
            const fn = getExport('wasm_tt_restore_display_list');
            return fn(slot >>> 0, BigInt(expectedKey || 0n)) === 1;
        },
        // --------------------------------------------------------------------
        // 5. SUPERFLUX BIáº¾N THIÃŠN QUANG PHá»” (SPECTRAL DIFFERENCE)
        // --------------------------------------------------------------------
        computeSuperflux(currBands, prevBands, numBands) {
            if (STATE.ready && STATE.sampleBufPtr && STATE.areaPtr) {
                ensureValidMemory();
                const n = Math.min(numBands, 512);
                const f32Idx = STATE.sampleBufPtr >> 2;
                STATE.views.f32.set(currBands.subarray(0, n), f32Idx);
                STATE.views.f32.set(prevBands.subarray(0, n), f32Idx + n);
                const fn = getExport('wasm_compute_superflux');
                if (fn) {
                    fn(STATE.sampleBufPtr, STATE.sampleBufPtr + (n << 2), n, STATE.areaPtr);
                    return STATE.views.f32[STATE.areaPtr >> 2];
                }
            }
            return null;
        },
        // --------------------------------------------------------------------
        // 6. LÆ¯á»šI BITBOARD 8x8 & KHÃ”NG GIAN (SPATIAL BITBOARD OCCLUSION)
        // --------------------------------------------------------------------
        spatialAabbMask(x0, y0, x1, y1, W, H) {
            const fn = getExport('wasm_spatial_aabb_mask');
            return BigInt.asUintN(64, fn(x0, y0, x1, y1, W, H));
        },
        // --------------------------------------------------------------------
        // 7. SHARED TELEMETRY MEMORY INTERFACE (CORE 60 FPS WRITE / DOM 10Hz READ)
        // DÃ¹ng chung 1 Ä‘á»‹a chá»‰ bá»™ nhá»› linear 256 bytes giá»¯a Render Core vÃ  DOM Inspector
        // --------------------------------------------------------------------
        getTelemetryViews: getTelemetryViews,
        writeTelemetry(data) {
            const v = getTelemetryViews();
            if (!v) return;
            if (data.fps !== undefined && v.f64[0] <= 0.0) v.f64[0] = data.fps;
            if (data.frameTimeMs !== undefined && v.f64[1] <= 0.0) v.f64[1] = data.frameTimeMs;
            if (data.cpuTimeMs !== undefined && v.f64[2] <= 0.0) v.f64[2] = data.cpuTimeMs;
            if (data.nowMs !== undefined) v.f64[3] = data.nowMs;
            if (data.currentHash !== undefined) {
                if (v.u64) {
                    v.u64[4] = BigInt(data.currentHash);
                } else {
                    const h = BigInt(data.currentHash);
                    v.u32[8] = Number(h & 0xFFFFFFFFn);
                    v.u32[9] = Number((h >> 32n) & 0xFFFFFFFFn);
                }
            }
            if (data.frameIndex !== undefined) v.u32[10] = data.frameIndex >>> 0;
            // renderMode is exclusively managed by C++ Native Core (single source of truth)
            if (data.dirtyN !== undefined) v.u32[12] = data.dirtyN >>> 0;
            if (data.culledCount !== undefined) v.u32[13] = data.culledCount >>> 0;
            if (data.activeFxCount !== undefined) v.u32[14] = data.activeFxCount >>> 0;
            if (data.activeLayersCount !== undefined) v.u32[15] = data.activeLayersCount >>> 0;
            if (data.styleEpoch !== undefined) v.u32[16] = data.styleEpoch >>> 0;
            if (data.fontsVer !== undefined) v.u32[17] = data.fontsVer >>> 0;
            if (data.mediaVer !== undefined) v.u32[18] = data.mediaVer >>> 0;
            if (data.baseW !== undefined) v.u32[19] = data.baseW >>> 0;
            if (data.baseH !== undefined) v.u32[20] = data.baseH >>> 0;
            if (data.previewW !== undefined) v.u32[21] = data.previewW >>> 0;
            if (data.previewH !== undefined) v.u32[22] = data.previewH >>> 0;
            if (data.scale !== undefined) v.f32[23] = data.scale;
            if (data.dpr !== undefined) v.f32[24] = data.dpr;
            if (data.ttHits !== undefined) v.u32[25] = data.ttHits >>> 0;
            if (data.ttMisses !== undefined) v.u32[26] = data.ttMisses >>> 0;
            if (data.zobristBreakers !== undefined) v.u32[27] = data.zobristBreakers >>> 0;
            if (data.dirtyAreaPx !== undefined) v.u32[28] = data.dirtyAreaPx >>> 0;
            if (data.isPlaying !== undefined) {
                v.u32[29] = (v.u32[29] & ~1) | (data.isPlaying ? 1 : 0);
            }
            v.u32[30] = (v.u32[30] + 1) >>> 0; // Heartbeat increment
        },
        get telemetry() {
            return this.readTelemetry();
        },
        readTelemetry(out) {
            if (!STATE.ready || !STATE.memory) return null;
            const v = getTelemetryViews();
            if (!v) return null;
            const t = out || {};
            t.fps = v.f64[0];
            t.frameTimeMs = v.f64[1];
            t.cpuTimeMs = v.f64[2];
            t.nowMs = v.f64[3];
            if (v.u64) {
                t.currentHash = v.u64[4];
            } else {
                t.currentHash = BigInt(v.u32[8]) | (BigInt(v.u32[9]) << 32n);
            }
            t.currentHashHex = '0x' + t.currentHash.toString(16).padStart(16, '0');
            t.frameIndex = v.u32[10];
            const mCode = v.u32[11];
            t.renderMode = RENDER_MODE_NAMES[mCode] || 'DIRTY';
            t.dirtyN = v.u32[12];
            t.culledCount = v.u32[13];
            t.activeFxCount = v.u32[14];
            t.activeLayersCount = v.u32[15];
            t.styleEpoch = v.u32[16];
            t.fontsVer = v.u32[17];
            t.mediaVer = v.u32[18];
            t.baseW = v.u32[19];
            t.baseH = v.u32[20];
            t.previewW = v.u32[21];
            t.previewH = v.u32[22];
            t.scale = v.f32[23];
            t.dpr = v.f32[24];
            t.ttHits = v.u32[25];
            t.ttMisses = v.u32[26];
            t.zobristBreakers = v.u32[27];
            t.dirtyAreaPx = v.u32[28];
            t.isPlaying = (v.u32[29] & 1) !== 0;
            t.heartbeat = v.u32[30];
            t.fullCount = v.u32[31];
            t.dirtyCount = v.u32[32];
            t.noneCount = v.u32[33];
            return t;
        },
        resetTelemetry() {
            const fn = getExport('wasm_telemetry_reset');
            fn();
        },
        getActiveLayers() {
            if (!STATE.ready || !STATE.memory) return [];
            ensureValidMemory();
            const getSlotsPtr = getExport('wasm_pos_get_evaluated_slots_ptr');
            if (!getSlotsPtr) return [];
            const ptr = getSlotsPtr();
            if (!ptr) return [];
            const getActiveMask = getExport('wasm_pos_get_active_mask');
            const activeMask = getActiveMask ? BigInt(getActiveMask()) : ~0n;
            const f32 = new Float32Array(STATE.memory.buffer, ptr, 64 * 16);
            const u32 = new Uint32Array(STATE.memory.buffer, ptr, 64 * 16);
            const u16 = new Uint16Array(STATE.memory.buffer, ptr, 64 * 32);
            const TYPE_NAMES = { 2: 'background', 3: 'text', 4: 'image', 5: 'video', 8: 'effect', 10: 'path' };
            const typeCounters = { bg: 0, img: 0, text: 0, lyric: 0, vid: 0, path: 0, layer: 0 };
            const layers = [];
            for (let i = 0; i < 64; i++) {
                const base32 = i * 16;
                const base16 = i * 32;
                const isVisible = u32[base32 + 10];
                if (!isVisible) continue;
                const origSlot = u16[base16 + 19];
                const slot = (origSlot !== undefined && origSlot < 64) ? origSlot : i;
                // Chỉ ghi nhận các slot thực sự nằm trong active bitboard của khung hình hiện tại
                if (getActiveMask && !(activeMask & (1n << BigInt(slot)))) continue;
                const x0 = f32[base32];
                const y0 = f32[base32 + 1];
                const x1 = f32[base32 + 2];
                const y1 = f32[base32 + 3];
                const w = Math.max(0, x1 - x0);
                const h = Math.max(0, y1 - y0);
                const opCode = u16[base16 + 16];
                const zRank = u16[base16 + 18];
                const custom0 = f32[base32 + 11];
                const tlItem = (global.state && global.state.timeline && global.state.timeline[slot]) || null;
                const rawType = TYPE_NAMES[opCode] || 'layer';
                const isPath = (opCode === 10) || (tlItem && tlItem.type === 'path');
                const isLyric = (tlItem && tlItem.type === 'lyric') || (opCode === 3 && tlItem && tlItem.type === 'lyric');
                const displayType = isPath ? 'path' : (isLyric ? 'lyric' : (rawType !== 'layer' ? rawType : (tlItem && tlItem.type ? tlItem.type : rawType)));
                // Sinh ID thông minh, ngắn gọn, trực quan (bg_1, img_1, text_1, lyric_1, path_1...)
                let smartId = '';
                if (tlItem && tlItem.id && typeof tlItem.id === 'string' && tlItem.id.trim()) {
                    smartId = tlItem.id.trim();
                } else {
                    const prefix = displayType === 'background' ? 'bg'
                        : displayType === 'image' ? 'img'
                        : displayType === 'video' ? 'vid'
                        : displayType === 'path' ? 'path'
                        : isLyric ? 'lyric' : (displayType === 'text' ? 'text' : 'layer');
                    typeCounters[prefix] = (typeCounters[prefix] || 0) + 1;
                    smartId = `${prefix}_${typeCounters[prefix]}`;
                }
                const isDynamic = (custom0 > 0.5) ||
                    (tlItem && (
                        (Array.isArray(tlItem.keyframes) && tlItem.keyframes.length > 1) ||
                        tlItem.spin || tlItem.spinSpeed ||
                        isLyric || tlItem.type === 'video'
                    ));
                layers.push({
                    id: smartId,
                    type: displayType,
                    z: zRank,
                    bounds: `[${Math.round(x0)}, ${Math.round(y0)}, ${Math.round(w)}, ${Math.round(h)}]`,
                    state: isDynamic ? 'dynamic' : 'static'
                });
            }
            const hasExplicitBg = layers.some(l => l.type === 'background');
            if (!hasExplicitBg) {
                const stageW = (global.state && global.state.renderConfig && global.state.renderConfig.baseW) || 1920;
                const stageH = (global.state && global.state.renderConfig && global.state.renderConfig.baseH) || 1080;
                layers.unshift({
                    id: 'bg_0',
                    type: 'background',
                    z: -1,
                    bounds: `[0, 0, ${Math.round(stageW)}, ${Math.round(stageH)}]`,
                    state: 'static'
                });
            }
            return layers;
        },
        getSlotBounds(slot) {
            if (!STATE.ready || !STATE.memory || slot < 0 || slot >= 64) return null;
            ensureValidMemory();
            const getSlotsPtr = getExport('wasm_pos_get_evaluated_slots_ptr');
            if (!getSlotsPtr) return null;
            const ptr = getSlotsPtr();
            if (!ptr) return null;
            const f32 = new Float32Array(STATE.memory.buffer, ptr, 64 * 16);
            const base = (slot >>> 0) * 16;
            const x0 = f32[base];
            const y0 = f32[base + 1];
            const x1 = f32[base + 2];
            const y1 = f32[base + 3];
            return { x: x0, y: y0, w: Math.max(0, x1 - x0), h: Math.max(0, y1 - y0) };
        },
        getActiveEffects() {
            if (!STATE.ready || !STATE.memory) return [];
            ensureValidMemory();
            const getPasses = getExport('wasm_timeline_runtime_get_passes');
            const getPassCount = getExport('wasm_timeline_runtime_get_pass_count');
            if (!getPasses || !getPassCount) return [];
            const count = getPassCount();
            if (!count || count <= 0 || count > 2048) return [];
            const ptr = getPasses();
            if (!ptr || (ptr + count * 40) > STATE.memory.buffer.byteLength) return [];
            const PASS_NAMES = {
                0: 'clear-scene',
                1: 'draw-layer',
                2: 'downsample',
                3: 'gaussian-h',
                4: 'gaussian-v',
                5: 'composite-blur',
                6: 'composite-glow',
                7: 'composite-scene-blur',
                8: 'brightness',
                9: 'copy-scene',
                10: 'zoom',
                11: 'shake',
                12: 'bloom-extract',
                13: 'bloom-down',
                14: 'bloom-up',
                15: 'bloom-composite',
                16: 'shader-effect'
            };
            const SHADER_EFFECT_NAMES = {
                0: 'none',
                1: 'gaussian-blur',
                2: 'glow',
                3: 'flicker',
                4: 'zoom',
                5: 'shake',
                6: 'bloom',
                7: 'intro',
                8: 'outro',
                9: 'glitch',
                10: 'matrix',
                11: 'rain-drop',
                17: 'turbulent',
                18: 'twirl',
                19: 'pinch',
                20: 'lens',
                21: 'chromatic',
                22: 'zoom-blur',
                23: 'motion-blur',
                24: 'glitch-rgb',
                25: 'cyber-scan',
                26: 'fire',
                27: 'rain-bokeh',
                28: 'film-burn',
                29: 'fxaa'
            };
            const u8 = new Uint8Array(STATE.memory.buffer, ptr, count * 40);
            const u32 = new Uint32Array(STATE.memory.buffer, ptr, count * 10);
            const f32 = new Float32Array(STATE.memory.buffer, ptr, count * 10);
            const getStart = getExport('wasm_pos_get_item_start_ms');
            const getEnd = getExport('wasm_pos_get_item_end_ms');
            const seenSlots = new Set();
            const effects = [];
            for (let i = 0; i < count; i++) {
                const kind = u8[i * 40];
                if (kind <= 1 || kind === 9) continue;
                const slot = u32[i * 10 + 1];
                if (seenSlots.has(slot)) continue;
                seenSlots.add(slot);
                const kernelIndex = u8[i * 40 + 3];
                let effectName = (kind === 16)
                    ? (SHADER_EFFECT_NAMES[kernelIndex] || 'shader-effect')
                    : (PASS_NAMES[kind] || `effect-${kind}`);
                const item = (global.state && global.state.timeline && global.state.timeline[slot]) || null;
                if (item && item.effect) {
                    effectName = item.effect;
                }
                let startMs = 0;
                let endMs = 0;
                if (item && item.start !== undefined && item.end !== undefined) {
                    startMs = Number(item.start) || 0;
                    endMs = Number(item.end) || 0;
                } else if (getStart && getEnd) {
                    startMs = Number(getStart(slot >>> 0));
                    endMs = Number(getEnd(slot >>> 0));
                }
                // Phân nhóm hiệu ứng theo chức năng kỹ thuật:
                // - transition: hiệu ứng chuyển tiếp (intro, outro, transition-*)
                // - element: hiệu ứng nội bộ của 1 phần tử (gắn trực tiếp vào text, image, div...)
                // - full-layer: hiệu ứng toàn layer / chèn theo layer (có z-index cụ thể < 9999 hoặc layer)
                // - full-screen: hiệu ứng toàn màn hình (phủ trọn viewport không giới hạn layer)
                const hasExplicitLayer = Boolean(
                    item && (item.layer !== undefined || (item['z-index'] !== undefined && item['z-index'] < 9999))
                );
                let groupName = 'full-screen';
                if (effectName === 'intro' || effectName === 'outro' || effectName.startsWith('transition-')) {
                    groupName = 'transition';
                } else if (item && item.type && item.type !== 'effect') {
                    groupName = 'element';
                } else if (hasExplicitLayer) {
                    groupName = 'full-layer';
                } else {
                    groupName = 'full-screen';
                }
                const zVal = hasExplicitLayer ? item['z-index'] : slot;
                let pipelineName = 'Ping-Pong';
                if ((kind >= 12 && kind <= 15) || effectName === 'bloom') {
                    pipelineName = 'MIP-Pyramid';
                } else if (kind === 3 || kind === 4 || kind === 5 || kind === 7) {
                    pipelineName = 'Dual-Pass';
                }
                effects.push({
                    name: effectName,
                    kind: `${pipelineName} (WebGL)`,
                    z: zVal,
                    slot: slot,
                    start: startMs,
                    end: endMs,
                    params: groupName,
                    group: groupName
                });
            }
            return effects;
        },
        recordFrameTime(deltaMs) {
            const fn = getExport('wasm_telemetry_record_frame_time');
            if (!fn) throw new Error('[WasmBridge] Native frame-time telemetry is unavailable.');
            return fn(deltaMs);
        },
        // --------------------------------------------------------------------
        // 8. BINARY DISPLAY LIST COMMAND BUFFER (INVERTED WASM PIPELINE)
        // --------------------------------------------------------------------
        getDisplayListViews: getDisplayListViews,
        displayList: {
            OPCODES: {
                NOP: 0,
                CLEAR_RECT: 1,
                DRAW_RECT: 2,
                DRAW_TEXT: 3,
                DRAW_IMAGE: 4,
                DRAW_VIDEO: 5,
                PUSH_CLIP: 6,
                POP_CLIP: 7,
                PUSH_FILTER: 8,
                POP_FILTER: 9
            },
            reset() {
                dlCount = 0;
                const fn = getExport('wasm_display_list_reset') || getExport('wasm_set_display_list_count');
                if (fn) fn(0);
            },
            getCount() {
                const getFn = getExport('wasm_get_display_list_count');
                if (getFn) return getFn();
                return dlCount;
            },
            setCount(n) {
                dlCount = Math.min(256, Math.max(0, n));
                const setFn = getExport('wasm_set_display_list_count');
                if (setFn) setFn(dlCount);
            },
            push(opCode, uid, x0, y0, x1, y1, opacity = 1.0, aux = 0, flags = 0) {
                const fnPush = getExport('wasm_display_list_push');
                const styleSlot = aux & 0xFFFF;
                const filterSlot = (aux >>> 16) & 0xFFFF;
                fnPush(opCode >>> 0, flags >>> 0, uid >>> 0, x0, y0, x1, y1, opacity, styleSlot, filterSlot);
                dlCount = this.getCount();
                return dlCount - 1;
            },
            generateFromScene(nodeCount, viewportW = 0, viewportH = 0) {
                ensureValidMemory();
                const fn = getExport('wasm_display_list_generate_from_scene');
                dlCount = fn(nodeCount >>> 0, viewportW, viewportH);
                return dlCount;
            },
            cull(occludedMask, W, H) {
                const cullFn = getExport('wasm_display_list_cull');
                return cullFn(BigInt(occludedMask), W, H);
            },
            getCommand(idx, outCmd) {
                ensureValidMemory();
                const v = getDisplayListViews();
                const u16 = v.u16;
                const u32 = v.u32;
                const f32 = v.f32;
                const base16 = idx << 4;
                const base32 = idx << 3;
                const out = outCmd || {};
                out.opCode = u16[base16 + 0];
                out.flags = u16[base16 + 1];
                out.resourceId = u32[base32 + 1];
                out.x0 = f32[base32 + 2];
                out.y0 = f32[base32 + 3];
                out.x1 = f32[base32 + 4];
                out.y1 = f32[base32 + 5];
                out.opacity = f32[base32 + 6];
                out.styleSlot = u16[base16 + 14];
                out.filterSlot = u16[base16 + 15];
                return out;
            }
        },
        // --------------------------------------------------------------------
        // 9. INVERTED PIPELINE MASTER EVALUATION (SINGLE BOUNDARY CROSSING)
        // --------------------------------------------------------------------
        evaluatePositionPipeline(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, lastFrameHash, occludedMask, nowMs, frameIndex, isPlaying) {
            return WasmBridge.manager.renderAt(
                W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
                lastFrameHash, occludedMask, nowMs, frameIndex, isPlaying
            );
        },
        evaluateFramePipeline(W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash, itemKeysBuf, count, lastFrameHash, occludedMask, nowMs, frameIndex, isPlaying) {
            ensureValidMemory();
            const u64Offset = STATE.itemKeysBufPtr >> 3;
            const n = Math.min(count, STATE.itemKeysCapacity);
            if (itemKeysBuf && n > 0 && STATE.views.u64) {
                STATE.views.u64.set(itemKeysBuf.subarray(0, n), u64Offset);
            }
            const fn = getExport('wasm_render_pipeline_evaluate');
            const resPtr = fn(
                W, H, styleEpoch, fontsVer, styleVer, mediaVer, bgHash,
                n, BigInt(lastFrameHash || 0), BigInt(occludedMask || 0),
                nowMs, frameIndex >>> 0, isPlaying ? 1 : 0
            );
            if (!resPtr) throw new Error('[WasmBridge FATAL] render_pipeline_evaluate returned null!');
            const u32Idx = resPtr >> 2;
            const u64Idx = resPtr >> 3;
            const resHash = STATE.views.u64 ? STATE.views.u64[u64Idx] : (BigInt(STATE.views.u32[u32Idx]) | (BigInt(STATE.views.u32[u32Idx + 1]) << 32n));
            return {
                currentHash: resHash,
                decision: STATE.views.u32[u32Idx + 2], // 0=DIRTY, 1=FULL, 2=ZOBRIST_SKIP, 3=TT_HIT
                ttData: STATE.views.u32[u32Idx + 3],
                ttTier: STATE.views.u32[u32Idx + 4],
                culledCount: STATE.views.u32[u32Idx + 5]
            };
        },
        // --------------------------------------------------------------------
        // 10. CSS-COMPLIANT NATIVE STYLE ENGINE (PACKEDSTYLE 128 BYTES & ZERO-GC COMPILER)
        // --------------------------------------------------------------------
        getStylesViews: getStylesViews,
        styleEngine: {
            BITS: {
                TRANSFORM: 1 << 0,
                OPACITY: 1 << 1,
                FILTER: 1 << 2,
                BORDER: 1 << 3,
                RADIUS: 1 << 4,
                TYPOGRAPHY: 1 << 5,
                ANIMATION: 1 << 6,
                SHADOW: 1 << 7,
                CLIP: 1 << 8,
                BACKGROUND: 1 << 9,
                VISIBLE: 1 << 10
            },
            reset(slotIdx) {
                if (slotIdx < 0 || slotIdx >= 256) return;
                const fn = getExport('wasm_style_reset');
                if (fn) {
                    fn(slotIdx);
                    return;
                }
                const v = getStylesViews();
                const base = slotIdx << 5;
                v.u32.fill(0, base, base + 32);
                v.f32[base + 6] = 1.0;
                v.f32[base + 7] = 1.0;
                v.f32[base + 13] = 1.0;
                v.u32[base + 15] = 1 << 10;
                v.u32[base + 16] = 0xFFFFFFFF;
                v.f32[base + 24] = 32.0;
                v.f32[base + 26] = 1.2;
                v.f32[base + 29] = 1.0;
                v.f32[base + 30] = 1.0;
            },
            compile(css, slotIdx = 0) {
                if (slotIdx < 0 || slotIdx >= 256) return -1;
                this.reset(slotIdx);
                if (!css || typeof css !== 'object') return slotIdx;
                const v = getStylesViews();
                const base = slotIdx << 5;
                let flags = 1 << 10; // VISIBLE
                // 1. Geometry (x, y, w, h)
                if (css.x !== undefined || css.left !== undefined) {
                    v.f32[base + 0] = parseFloat(css.x !== undefined ? css.x : css.left) || 0;
                }
                if (css.y !== undefined || css.top !== undefined) {
                    v.f32[base + 1] = parseFloat(css.y !== undefined ? css.y : css.top) || 0;
                }
                if (css.width !== undefined || css.w !== undefined) {
                    v.f32[base + 2] = parseFloat(css.width !== undefined ? css.width : css.w) || 0;
                }
                if (css.height !== undefined || css.h !== undefined) {
                    v.f32[base + 3] = parseFloat(css.height !== undefined ? css.height : css.h) || 0;
                }
                // 2. Transforms (translate, scale, rotate, skew, origin)
                let hasTransform = false;
                if (css.transform) {
                    hasTransform = true;
                    if (typeof css.transform === 'string') {
                        const tObj = { translateX: 0, translateY: 0, scaleX: 1, scaleY: 1, rotateDeg: 0, skewX: 0, skewY: 0 };
                        parseCssTransform(css.transform, tObj);
                        v.f32[base + 4] = tObj.translateX;
                        v.f32[base + 5] = tObj.translateY;
                        v.f32[base + 6] = tObj.scaleX;
                        v.f32[base + 7] = tObj.scaleY;
                        v.f32[base + 8] = tObj.rotateDeg;
                        v.f32[base + 9] = tObj.skewX;
                        v.f32[base + 10] = tObj.skewY;
                    } else if (typeof css.transform === 'object') {
                        if (css.transform.translate) {
                            v.f32[base + 4] = parseFloat(css.transform.translate[0]) || 0;
                            v.f32[base + 5] = parseFloat(css.transform.translate[1]) || 0;
                        }
                        if (css.transform.scale !== undefined) {
                            const s = css.transform.scale;
                            v.f32[base + 6] = Array.isArray(s) ? (parseFloat(s[0]) || 1) : (parseFloat(s) || 1);
                            v.f32[base + 7] = Array.isArray(s) ? (parseFloat(s[1]) || 1) : (parseFloat(s) || 1);
                        }
                        if (css.transform.rotate !== undefined) {
                            v.f32[base + 8] = parseFloat(css.transform.rotate) || 0;
                        }
                    }
                }
                if (css.translateX !== undefined) { v.f32[base + 4] = parseFloat(css.translateX) || 0; hasTransform = true; }
                if (css.translateY !== undefined) { v.f32[base + 5] = parseFloat(css.translateY) || 0; hasTransform = true; }
                if (css.scaleX !== undefined) { v.f32[base + 6] = parseFloat(css.scaleX) || 1; hasTransform = true; }
                if (css.scaleY !== undefined) { v.f32[base + 7] = parseFloat(css.scaleY) || 1; hasTransform = true; }
                if (css.rotate !== undefined || css.rotateDeg !== undefined) {
                    v.f32[base + 8] = parseFloat(css.rotate !== undefined ? css.rotate : css.rotateDeg) || 0;
                    hasTransform = true;
                }
                if (hasTransform) flags |= (1 << 0); // STYLE_BIT_TRANSFORM
                // Transform Origin
                if (css.transformOrigin) {
                    if (Array.isArray(css.transformOrigin)) {
                        v.f32[base + 11] = parseFloat(css.transformOrigin[0]) || 0;
                        v.f32[base + 12] = parseFloat(css.transformOrigin[1]) || 0;
                    } else if (typeof css.transformOrigin === 'string') {
                        const parts = css.transformOrigin.split(' ');
                        v.f32[base + 11] = parseFloat(parts[0]) || 0;
                        v.f32[base + 12] = parseFloat(parts[1]) || 0;
                    }
                }
                // 3. Opacity & Z-Index
                if (css.opacity !== undefined) {
                    v.f32[base + 13] = Math.min(1.0, Math.max(0.0, parseFloat(css.opacity) || 0));
                    flags |= (1 << 1); // STYLE_BIT_OPACITY
                }
                if (css.zIndex !== undefined) {
                    v.i32[base + 14] = parseInt(css.zIndex, 10) || 0;
                }
                // 4. Colors
                if (css.color !== undefined) {
                    v.u32[base + 16] = WasmBridge.manager.parseTimelineColor(css.color, 0xFFFFFFFF);
                }
                if (css.backgroundColor !== undefined || css['background-color'] !== undefined || css.bg !== undefined) {
                    const bg = css.backgroundColor || css['background-color'] || css.bg;
                    v.u32[base + 17] = WasmBridge.manager.parseTimelineColor(bg, 0);
                    flags |= (1 << 9); // STYLE_BIT_BACKGROUND
                }
                if (css.borderColor !== undefined || css['border-color'] !== undefined) {
                    v.u32[base + 18] = WasmBridge.manager.parseTimelineColor(css.borderColor || css['border-color'], 0);
                    flags |= (1 << 3); // STYLE_BIT_BORDER
                }
                if (css.borderWidth !== undefined || css['border-width'] !== undefined) {
                    v.f32[base + 19] = parseFloat(css.borderWidth || css['border-width']) || 0;
                    flags |= (1 << 3);
                }
                // 5. Border Radius
                const r = css.borderRadius !== undefined ? css.borderRadius : css['border-radius'];
                if (r !== undefined) {
                    flags |= (1 << 4); // STYLE_BIT_RADIUS
                    if (typeof r === 'number') {
                        v.f32[base + 20] = r; v.f32[base + 21] = r;
                        v.f32[base + 22] = r; v.f32[base + 23] = r;
                    } else if (Array.isArray(r)) {
                        v.f32[base + 20] = parseFloat(r[0]) || 0;
                        v.f32[base + 21] = parseFloat(r[1]) || v.f32[base + 20];
                        v.f32[base + 22] = parseFloat(r[2]) || v.f32[base + 20];
                        v.f32[base + 23] = parseFloat(r[3]) || v.f32[base + 21];
                    }
                }
                // 6. Typography
                if (css.fontSize !== undefined || css['font-size'] !== undefined) {
                    v.f32[base + 24] = parseFloat(css.fontSize || css['font-size']) || 32;
                    flags |= (1 << 5); // STYLE_BIT_TYPOGRAPHY
                }
                if (css.letterSpacing !== undefined || css['letter-spacing'] !== undefined) {
                    v.f32[base + 25] = parseFloat(css.letterSpacing || css['letter-spacing']) || 0;
                    flags |= (1 << 5);
                }
                if (css.lineHeight !== undefined || css['line-height'] !== undefined) {
                    v.f32[base + 26] = parseFloat(css.lineHeight || css['line-height']) || 1.2;
                    flags |= (1 << 5);
                }
                let weight = 400, fStyle = 0, align = 0;
                if (css.fontWeight || css['font-weight']) {
                    const fw = String(css.fontWeight || css['font-weight']).toLowerCase();
                    if (fw === 'bold' || fw === '700') weight = 700;
                    else if (fw === '600') weight = 600;
                    else if (fw === '300' || fw === 'light') weight = 300;
                    else weight = parseInt(fw, 10) || 400;
                }
                if (css.textAlign || css['text-align']) {
                    const ta = String(css.textAlign || css['text-align']).toLowerCase();
                    if (ta === 'center') align = 1;
                    else if (ta === 'right') align = 2;
                    else if (ta === 'justify') align = 3;
                }
                v.u32[base + 27] = ((weight & 0xFFFF) << 16) | ((fStyle & 0xFF) << 8) | (align & 0xFF);
                // 7. Filters & Blend Modes
                if (css.filter) {
                    flags |= (1 << 2); // STYLE_BIT_FILTER
                    const fObj = { filterBlur: 0, filterBright: 1, filterContrast: 1 };
                    parseCssFilter(css.filter, fObj);
                    v.f32[base + 28] = fObj.filterBlur;
                    v.f32[base + 29] = fObj.filterBright;
                    v.f32[base + 30] = fObj.filterContrast;
                }
                let blend = 0;
                const bm = css.mixBlendMode || css['mix-blend-mode'];
                if (bm) {
                    if (bm === 'screen') blend = 1;
                    else if (bm === 'multiply') blend = 2;
                    else if (bm === 'overlay') blend = 3;
                    else if (bm === 'add') blend = 4;
                }
                v.u32[base + 31] = (blend & 0xFF);
                v.u32[base + 15] = flags >>> 0;
                return slotIdx;
            },
            computeAffine2D(slotIdx, out6Floats) {
                const out = out6Floats || new Float32Array(6);
                const fn = getExport('wasm_style_compute_affine_2d');
                fn(slotIdx, STATE.areaPtr);
                out.set(STATE.views.f32.subarray(STATE.areaPtr >> 2, (STATE.areaPtr >> 2) + 6));
                return out;
            },
            interpolate(fromSlot, toSlot, t, outSlot) {
                const fn = getExport('wasm_style_interpolate');
                fn(fromSlot, toSlot, t, outSlot);
                return outSlot;
            },
            hash(slotIdx) {
                const fn = getExport('wasm_style_hash');
                if (fn) {
                    return BigInt.asUintN(64, fn(slotIdx));
                }
                const v = getStylesViews();
                const b = slotIdx << 5;
                let h = 0x9e3779b97f4a7c15n;
                for (let i = 0; i < 32; i += 2) {
                    const w = BigInt(v.u32[b + i]) | (BigInt(v.u32[b + i + 1]) << 32n);
                    h ^= w * 0x517cc1b727220a95n;
                    h = BigInt.asUintN(64, (h << 31n) | (h >> 33n));
                }
                return h === 0n ? 1n : h;
            }
        },
        // --------------------------------------------------------------------
        // 11. DOM-INDEPENDENT VECTOR/MSDF TYPOGRAPHY & WORD-WRAPPING ENGINE
        // Bá»‘ cá»¥c vÄƒn báº£n, ngáº¯t dÃ²ng, cÄƒn lá» vÃ  Ä‘á»‹nh vá»‹ kÃ½ tá»± thuáº§n sá»‘ há»c trong C++ Wasm
        // KhÃ´ng Ä‘á»¥ng vÃ o DOM layout, khÃ´ng kÃ­ch hoáº¡t Browser Reflow, 0-Allocation
        // --------------------------------------------------------------------
        textLayout: {
            FONT_CAT_SANS: 0,
            FONT_CAT_SERIF: 1,
            FONT_CAT_MONO: 2,
            FONT_CAT_CONDENSED: 3,
            FONT_CAT_SCRIPT: 4,
            ALIGN_LEFT: 0,
            ALIGN_CENTER: 1,
            ALIGN_RIGHT: 2,
            ALIGN_JUSTIFY: 3,
            TRANSFORM_NONE: 0,
            TRANSFORM_UPPERCASE: 1,
            TRANSFORM_LOWERCASE: 2,
            parseFontCategory(fontFamily) {
                if (!fontFamily || typeof fontFamily !== 'string') return 0;
                const lower = fontFamily.toLowerCase();
                if (lower.includes('anton') || lower.includes('bebas') || lower.includes('teko') || lower.includes('condensed')) return 3;
                if (lower.includes('playfair') || lower.includes('times') || lower.includes('serif')) return 1;
                if (lower.includes('mono') || lower.includes('courier') || lower.includes('consolas')) return 2;
                if (lower.includes('script') || lower.includes('pacifico') || lower.includes('dancing') || lower.includes('cursive') || lower.includes('vibes')) return 4;
                return 0; // Sans
            },
            parseAlign(align) {
                if (!align || typeof align !== 'string') return 0;
                const lower = align.toLowerCase();
                if (lower === 'center') return 1;
                if (lower === 'right') return 2;
                if (lower === 'justify') return 3;
                return 0;
            },
            parseTransform(transform) {
                if (!transform || typeof transform !== 'string') return 0;
                const lower = transform.toLowerCase();
                if (lower === 'uppercase') return 1;
                if (lower === 'lowercase') return 2;
                return 0;
            },
            layout(text, options = {}) {
                ensureValidMemory();
                const fontSize = typeof options.fontSize === 'number' ? options.fontSize : (parseFloat(options.fontSize) || 60.0);
                const maxWidth = typeof options.maxWidth === 'number' ? options.maxWidth : (parseFloat(options.maxWidth) || 0.0);
                const letterSpacing = typeof options.letterSpacing === 'number' ? options.letterSpacing : (parseFloat(options.letterSpacing) || 0.0);
                const lineHeight = typeof options.lineHeight === 'number' ? options.lineHeight : (parseFloat(options.lineHeight) || 1.2);
                const textAlign = typeof options.textAlign === 'number' ? options.textAlign : this.parseAlign(options.textAlign);
                const fontCat = typeof options.fontCategory === 'number' ? options.fontCategory : this.parseFontCategory(options.fontFamily);
                const transform = typeof options.textTransform === 'number' ? options.textTransform : this.parseTransform(options.textTransform);
                const str = String(text != null ? text : '');
                const charCount = Math.min(str.length, 4096);
                const inputView = getTextInputView();
                for (let i = 0; i < charCount; i++) {
                    inputView[i] = str.charCodeAt(i);
                }
                const fn = getExport('wasm_text_layout');
                fn(charCount, fontSize, maxWidth, letterSpacing, lineHeight, textAlign, fontCat, transform);
                const resView = getTextResultViews();
                return {
                    boundingWidth: resView.f32[0],
                    boundingHeight: resView.f32[1],
                    lineCount: resView.u32[2],
                    glyphCount: resView.u32[3],
                    wordCount: resView.u32[4],
                    firstBaseline: resView.f32[5]
                };
            },
            getGlyphCount() {
                const res = getTextResultViews();
                return res ? res.u32[3] : 0;
            },
            getResult() {
                const res = getTextResultViews();
                return {
                    boundingWidth: res.f32[0],
                    boundingHeight: res.f32[1],
                    lineCount: res.u32[2],
                    glyphCount: res.u32[3],
                    wordCount: res.u32[4],
                    firstBaseline: res.f32[5]
                };
            },
            getGlyph(index, out) {
                const glyphs = getTextGlyphsViews();
                const base = index << 3;
                const obj = out || {};
                obj.x = glyphs.f32[base + 0];
                obj.y = glyphs.f32[base + 1];
                obj.w = glyphs.f32[base + 2];
                obj.h = glyphs.f32[base + 3];
                obj.unicode = glyphs.u32[base + 4];
                const word5 = glyphs.u32[base + 5];
                obj.lineIndex = word5 & 0xFFFF;
                obj.charIndex = (word5 >>> 16) & 0xFFFF;
                obj.advance = glyphs.f32[base + 6];
                obj.flags = glyphs.u32[base + 7];
                return obj;
            },
            registerGlyphMetric(fontCategory, unicode, advance) {
                const fn = getExport('wasm_text_register_glyph_metric');
                if (fn) {
                    fn(fontCategory >>> 0, unicode >>> 0, advance);
                }
            },
            hash(text, options = {}) {
                const fontSize = typeof options.fontSize === 'number' ? options.fontSize : (parseFloat(options.fontSize) || 60.0);
                const maxWidth = typeof options.maxWidth === 'number' ? options.maxWidth : (parseFloat(options.maxWidth) || 0.0);
                const letterSpacing = typeof options.letterSpacing === 'number' ? options.letterSpacing : (parseFloat(options.letterSpacing) || 0.0);
                const lineHeight = typeof options.lineHeight === 'number' ? options.lineHeight : (parseFloat(options.lineHeight) || 1.2);
                const textAlign = typeof options.textAlign === 'number' ? options.textAlign : this.parseAlign(options.textAlign);
                const str = String(text != null ? text : '');
                const charCount = Math.min(str.length, 4096);
                const inputView = getTextInputView();
                for (let i = 0; i < charCount; i++) {
                    inputView[i] = str.charCodeAt(i);
                }
                const fn = getExport('wasm_text_layout_hash');
                if (fn) {
                    return BigInt.asUintN(64, fn(charCount, fontSize, maxWidth, letterSpacing, lineHeight, textAlign));
                }
                return BigInt(strHash(str) ^ (fontSize | 0));
            },
            getViews() {
                return {
                    inputView: getTextInputView(),
                    glyphsViews: getTextGlyphsViews(),
                    resultViews: getTextResultViews()
                };
            }
        },
        // --------------------------------------------------------------------
        // 12. HIERARCHICAL 2D SCENE GRAPH & SIMD128 VECTORIZED AABB ENGINE
        // TÃ­nh toÃ¡n ma tráº­n phÃ¢n cáº¥p cÃ¢y vÃ  AABB 4 Ä‘á»‰nh song song báº±ng SIMD128
        // Cáº¯t tá»‰a viewport vÃ  scissor clip box 0-allocation branchless
        // --------------------------------------------------------------------
        geometryEngine: {
            FLAGS: {
                VISIBLE: 1 << 0,
                DIRTY: 1 << 1,
                CULLED: 1 << 2,
                HAS_CLIP: 1 << 3,
                OPAQUE: 1 << 4
            },
            resetNode(idx) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_geometry_reset_node');
                if (fn) {
                    fn(idx);
                    return;
                }
                const v = getGeometryNodesViews();
                const baseF = idx << 4;
                const base16 = idx << 5;
                v.f32[baseF + 0] = 1; v.f32[baseF + 1] = 0;
                v.f32[baseF + 2] = 0; v.f32[baseF + 3] = 1;
                v.f32[baseF + 4] = 0; v.f32[baseF + 5] = 0;
                v.f32[baseF + 6] = 0; v.f32[baseF + 7] = 0;
                v.f32[baseF + 8] = 0; v.f32[baseF + 9] = 0;
                v.f32[baseF + 10] = 0; v.f32[baseF + 11] = 0;
                v.f32[baseF + 12] = 0; v.f32[baseF + 13] = 0;
                v.u16[base16 + 28] = 0xFFFF;
                v.u16[base16 + 29] = idx;
                v.u16[base16 + 30] = 0xFFFF;
                v.u16[base16 + 31] = 1 | 2;
            },
            setNodeLocal(idx, x, y, w, h, parentIdx = 0xFFFF, styleSlot = idx, clipNode = 0xFFFF, flags = 1) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_geometry_set_node_local');
                if (fn) {
                    fn(idx, x, y, w, h, parentIdx, styleSlot, clipNode, flags);
                    return;
                }
                const v = getGeometryNodesViews();
                const baseF = idx << 4;
                const base16 = idx << 5;
                v.f32[baseF + 10] = x;
                v.f32[baseF + 11] = y;
                v.f32[baseF + 12] = w;
                v.f32[baseF + 13] = h;
                v.u16[base16 + 28] = parentIdx;
                v.u16[base16 + 29] = styleSlot;
                v.u16[base16 + 30] = clipNode;
                v.u16[base16 + 31] = flags | 2;
            },
            evaluateScene(nodeCount, viewportW = 0, viewportH = 0) {
                ensureValidMemory();
                const n = Math.min(256, Math.max(0, nodeCount));
                const fn = getExport('wasm_geometry_evaluate_scene');
                fn(n, viewportW, viewportH);
                const res = getGeometryResultViews();
                return {
                    totalNodes: res.u32[0],
                    visibleNodes: res.u32[1],
                    culledNodes: res.u32[2],
                    clippedNodes: res.u32[3],
                    sceneMinX: res.f32[4],
                    sceneMinY: res.f32[5],
                    sceneMaxX: res.f32[6],
                    sceneMaxY: res.f32[7]
                };
            },
            getNode(idx, out) {
                if (idx < 0 || idx >= 256) return null;
                const v = getGeometryNodesViews();
                const baseF = idx << 4;
                const base16 = idx << 5;
                const obj = out || {};
                obj.worldMat = obj.worldMat || new Float32Array(6);
                obj.worldMat[0] = v.f32[baseF + 0];
                obj.worldMat[1] = v.f32[baseF + 1];
                obj.worldMat[2] = v.f32[baseF + 2];
                obj.worldMat[3] = v.f32[baseF + 3];
                obj.worldMat[4] = v.f32[baseF + 4];
                obj.worldMat[5] = v.f32[baseF + 5];
                obj.aabb = obj.aabb || new Float32Array(4);
                obj.aabb[0] = v.f32[baseF + 6];
                obj.aabb[1] = v.f32[baseF + 7];
                obj.aabb[2] = v.f32[baseF + 8];
                obj.aabb[3] = v.f32[baseF + 9];
                obj.local = obj.local || new Float32Array(4);
                obj.local[0] = v.f32[baseF + 10];
                obj.local[1] = v.f32[baseF + 11];
                obj.local[2] = v.f32[baseF + 12];
                obj.local[3] = v.f32[baseF + 13];
                obj.parentIndex = v.u16[base16 + 28];
                obj.styleSlot = v.u16[base16 + 29];
                obj.clipNode = v.u16[base16 + 30];
                obj.flags = v.u16[base16 + 31];
                return obj;
            },
            computeAABB(a, b, c, d, tx, ty, lx, ly, lw, lh, outAABB) {
                const out = outAABB || new Float32Array(4);
                const fn = getExport('wasm_geometry_compute_aabb_simd');
                if (fn && STATE.areaPtr) {
                    fn(a, b, c, d, tx, ty, lx, ly, lw, lh, STATE.areaPtr);
                    out.set(STATE.views.f32.subarray(STATE.areaPtr >> 2, (STATE.areaPtr >> 2) + 4));
                    return out;
                }
                const x0 = a * lx + c * ly + tx;
                const y0 = b * lx + d * ly + ty;
                const x1 = a * (lx + lw) + c * ly + tx;
                const y1 = b * (lx + lw) + d * ly + ty;
                const x2 = a * (lx + lw) + c * (ly + lh) + tx;
                const y2 = b * (lx + lw) + d * (ly + lh) + ty;
                const x3 = a * lx + c * (ly + lh) + tx;
                const y3 = b * lx + d * (ly + lh) + ty;
                out[0] = Math.min(x0, x1, x2, x3);
                out[1] = Math.min(y0, y1, y2, y3);
                out[2] = Math.max(x0, x1, x2, x3);
                out[3] = Math.max(y0, y1, y2, y3);
                return out;
            },
            getViews() {
                return {
                    nodesViews: getGeometryNodesViews(),
                    resultViews: getGeometryResultViews()
                };
            }
        },
        // --------------------------------------------------------------------
        // 7. SHADER PIPELINE & CSS FILTER / BLEND MODE (PHA 4)
        // --------------------------------------------------------------------
        filterPipeline: {
            BLEND_MODES: {
                NORMAL: 0,
                MULTIPLY: 1,
                SCREEN: 2,
                OVERLAY: 3,
                DARKEN: 4,
                LIGHTEN: 5,
                COLOR_DODGE: 6,
                COLOR_BURN: 7,
                HARD_LIGHT: 8,
                SOFT_LIGHT: 9,
                DIFFERENCE: 10,
                EXCLUSION: 11
            },
            FILTER_BITS: {
                BLUR: 1 << 0,
                SHADOW: 1 << 1,
                BRIGHTNESS: 1 << 2,
                CONTRAST: 1 << 3,
                SATURATE: 1 << 4,
                HUE_ROTATE: 1 << 5,
                INVERT: 1 << 6,
                SEPIA: 1 << 7,
                BLEND_MODE: 1 << 8
            },
            reset(idx) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_filter_reset');
                if (fn) { fn(idx); return; }
                const v = getFilterViews();
                const baseF = idx << 4;
                v.f32.fill(0, baseF, baseF + 16);
                v.f32[baseF + 6] = 1.0; // bright
                v.f32[baseF + 7] = 1.0; // contrast
                v.f32[baseF + 8] = 1.0; // sat
                v.f32[baseF + 12] = 1.0; // opacity
            },
            setBlur(idx, radius) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_filter_set_blur');
                if (fn) { fn(idx, radius); return; }
                const v = getFilterViews();
                const baseF = idx << 4;
                v.f32[baseF + 0] = Math.max(0, radius);
                if (v.f32[baseF + 0] > 0.01) v.u32[baseF + 14] |= (1 << 0);
                else v.u32[baseF + 14] &= ~(1 << 0);
            },
            setShadow(idx, ox, oy, blur, spread = 0, colorRgba = 0x80000000) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_filter_set_shadow');
                if (fn) { fn(idx, ox, oy, blur, spread, colorRgba >>> 0); return; }
                const v = getFilterViews();
                const baseF = idx << 4;
                v.f32[baseF + 1] = ox;
                v.f32[baseF + 2] = oy;
                v.f32[baseF + 3] = Math.max(0, blur);
                v.u32[baseF + 4] = colorRgba >>> 0;
                v.f32[baseF + 13] = spread;
                if (blur > 0.01 || spread > 0.01 || ox !== 0 || oy !== 0) v.u32[baseF + 14] |= (1 << 1);
                else v.u32[baseF + 14] &= ~(1 << 1);
            },
            setBlendMode(idx, blendMode) {
                if (idx < 0 || idx >= 256) return;
                const m = (blendMode >= 0 && blendMode <= 11) ? blendMode : 0;
                const fn = getExport('wasm_filter_set_blend_mode');
                if (fn) { fn(idx, m); return; }
                const v = getFilterViews();
                const base = idx << 4;
                v.u32[base + 5] = m;
                if (m > 0) v.u32[base + 14] |= (1 << 8);
                else v.u32[base + 14] &= ~(1 << 8);
            },
            setColorAdjustments(idx, bright = 1.0, contrast = 1.0, sat = 1.0, hueDeg = 0.0, inv = 0.0, sep = 0.0) {
                if (idx < 0 || idx >= 256) return;
                const fn = getExport('wasm_filter_set_color_adjustments');
                if (fn) { fn(idx, bright, contrast, sat, hueDeg, inv, sep); return; }
                const v = getFilterViews();
                const baseF = idx << 4;
                v.f32[baseF + 6] = bright;
                v.f32[baseF + 7] = contrast;
                v.f32[baseF + 8] = sat;
                v.f32[baseF + 9] = hueDeg;
                v.f32[baseF + 10] = inv;
                v.f32[baseF + 11] = sep;
                let bits = v.u32[baseF + 14] & ((1 << 0) | (1 << 1) | (1 << 8));
                if (bright !== 1.0) bits |= (1 << 2);
                if (contrast !== 1.0) bits |= (1 << 3);
                if (sat !== 1.0) bits |= (1 << 4);
                if (hueDeg !== 0.0) bits |= (1 << 5);
                if (inv > 0.0) bits |= (1 << 6);
                if (sep > 0.0) bits |= (1 << 7);
                v.u32[baseF + 14] = bits;
            },
            computeColorMatrix(idx, out16Floats) {
                const out = out16Floats || new Float32Array(16);
                const fn = getExport('wasm_filter_compute_color_matrix');
                ensureValidMemory();
                fn(idx, STATE.areaPtr);
                out.set(STATE.views.f32.subarray(STATE.areaPtr >> 2, (STATE.areaPtr >> 2) + 16));
                return out;
            },
            computeColorMatrixDirect(bright = 1.0, contrast = 1.0, sat = 1.0, hueDeg = 0.0, inv = 0.0, sep = 0.0, opacity = 1.0, out16Floats) {
                const out = out16Floats || new Float32Array(16);
                const fn = getExport('wasm_color_matrix_compute');
                ensureValidMemory();
                fn(bright, contrast, sat, hueDeg, inv, sep, opacity, STATE.areaPtr);
                out.set(STATE.views.f32.subarray(STATE.areaPtr >> 2, (STATE.areaPtr >> 2) + 16));
                return out;
            },
            computeKawasePasses(blurRadius, srcW, srcH, maxPasses = 8) {
                if (blurRadius <= 0.5 || srcW <= 0 || srcH <= 0) {
                    _reusableKawasePassesResult.length = 0;
                    return _reusableKawasePassesResult;
                }
                const fnStatic = getExport('wasm_filter_compute_kawase_passes_static');
                ensureValidMemory();
                const count = fnStatic(blurRadius, srcW >>> 0, srcH >>> 0);
                const v = getKawasePassesViews();
                const n = Math.min(count, maxPasses);
                _reusableKawasePassesResult.length = n;
                for (let i = 0; i < n; i++) {
                    const p = i * 4;
                    const item = _cachedKawasePassesPool[i];
                    item.offset = v.f32[p + 0];
                    item.targetWidth = v.u32[p + 1];
                    item.targetHeight = v.u32[p + 2];
                    item.isDownsample = v.u32[p + 3] === 1;
                    _reusableKawasePassesResult[i] = item;
                }
                return _reusableKawasePassesResult;
            },
            hash(idx) {
                if (idx < 0 || idx >= 256) return 0n;
                const fn = getExport('wasm_filter_hash');
                if (fn) {
                    return BigInt.asUintN(64, fn(idx));
                }
                const v = getFilterViews();
                const baseF = idx << 4;
                let h = 0x9e3779b97f4a7c15n;
                for (let i = 0; i < 16; i++) {
                    h = (h ^ BigInt(v.u32[baseF + i])) * 0x517cc1b727220a95n;
                    h = BigInt.asUintN(64, (h << 31n) | (h >> 33n));
                }
                return h === 0n ? 1n : h;
            },
            parseCssMixBlendMode(modeStr) {
                if (!modeStr || typeof modeStr !== 'string') return 0;
                const s = modeStr.trim().toLowerCase().replace(/-/g, '_');
                const modes = {
                    'normal': 0, 'multiply': 1, 'screen': 2, 'overlay': 3,
                    'darken': 4, 'lighten': 5, 'color_dodge': 6, 'color_burn': 7,
                    'hard_light': 8, 'soft_light': 9, 'difference': 10, 'exclusion': 11
                };
                return modes[s] !== undefined ? modes[s] : 0;
            },
            parseCssFilter(filterStr, slotIdx) {
                if (slotIdx < 0 || slotIdx >= 256) return;
                this.reset(slotIdx);
                if (!filterStr || typeof filterStr !== 'string' || filterStr === 'none') return;
                let blur = 0, bright = 1.0, contrast = 1.0, sat = 1.0, hueDeg = 0.0, inv = 0.0, sep = 0.0;
                let ox = 0, oy = 0, sBlur = 0, sSpread = 0, sColor = 0x80000000;
                let hasShadow = false;
                const reg = /([a-z\-]+)\(([^)]+)\)/gi;
                let m;
                while ((m = reg.exec(filterStr)) !== null) {
                    const fn = m[1].toLowerCase();
                    const rawArgs = m[2].trim();
                    if (fn === 'blur') {
                        blur = parseFloat(rawArgs) || 0;
                    } else if (fn === 'brightness') {
                        bright = parseFloat(rawArgs);
                        if (rawArgs.includes('%')) bright /= 100.0;
                    } else if (fn === 'contrast') {
                        contrast = parseFloat(rawArgs);
                        if (rawArgs.includes('%')) contrast /= 100.0;
                    } else if (fn === 'saturate') {
                        sat = parseFloat(rawArgs);
                        if (rawArgs.includes('%')) sat /= 100.0;
                    } else if (fn === 'hue-rotate') {
                        hueDeg = parseFloat(rawArgs) || 0;
                        if (rawArgs.includes('rad')) hueDeg *= (180.0 / Math.PI);
                    } else if (fn === 'invert') {
                        inv = parseFloat(rawArgs);
                        if (rawArgs.includes('%')) inv /= 100.0;
                    } else if (fn === 'sepia') {
                        sep = parseFloat(rawArgs);
                        if (rawArgs.includes('%')) sep /= 100.0;
                    } else if (fn === 'drop-shadow') {
                        hasShadow = true;
                        const parts = rawArgs.split(/\s+/);
                        const nums = parts.map(p => parseFloat(p)).filter(n => Number.isFinite(n));
                        ox = nums[0] || 0;
                        oy = nums[1] || 0;
                        sBlur = nums[2] || 0;
                        sSpread = nums[3] || 0;
                        const colorPart = parts.find(p => p.startsWith('#') || p.startsWith('rgb'));
                        if (colorPart) sColor = WasmBridge.manager.parseTimelineColor(colorPart, 0x80000000);
                    }
                }
                if (blur > 0) this.setBlur(slotIdx, blur);
                if (hasShadow) this.setShadow(slotIdx, ox, oy, sBlur, sSpread, sColor);
                this.setColorAdjustments(slotIdx, bright, contrast, sat, hueDeg, inv, sep);
            },
            getViews() {
                return getFilterViews();
            }
        },
        // --------------------------------------------------------------------
        // 14. THáº¾ TRáº¬N Ká»ŠCH Báº¢N & STATEINFO (TIMELINE POSITION SUBSYSTEM)
        // Ãp dá»¥ng mÃ´ hÃ¬nh Position & StateInfo tá»« Stockfish
        // --------------------------------------------------------------------
        position: {
            clear() {
                getExport('wasm_pos_clear')();
            },
            // ================================================================
            // WARM PATH: DIRECT ATOMIC IN-PLACE MUTATION (< 100ns, Zero JSON)
            // ================================================================
            mutateRegF32(slot, regIdx, val) {
                const fn = getExport('wasm_pos_mutate_reg_f32');
                if (fn) fn(slot >>> 0, regIdx >>> 0, Number(val));
            },
            mutateRegU32(slot, regIdx, val) {
                const fn = getExport('wasm_pos_mutate_reg_u32');
                if (fn) fn(slot >>> 0, regIdx >>> 0, val >>> 0);
            },
            mutateBit(slot, bitIdx, val) {
                const fn = getExport('wasm_pos_mutate_bit');
                if (fn) fn(slot >>> 0, bitIdx >>> 0, val ? 1 : 0);
            },
            getSlotRegF32(slot, regIdx) {
                const fn = getExport('wasm_pos_get_slot_reg_f32');
                return fn ? fn(slot >>> 0, regIdx >>> 0) : 0;
            },
            getSlotRegU32(slot, regIdx) {
                const fn = getExport('wasm_pos_get_slot_reg_u32');
                return fn ? fn(slot >>> 0, regIdx >>> 0) : 0;
            },
            getSlotBit(slot, bitIdx) {
                const fn = getExport('wasm_pos_get_slot_bit');
                return fn ? (fn(slot >>> 0, bitIdx >>> 0) !== 0) : false;
            },
            mutateFxParamF32(instanceSlot, regIdx, val) {
                const fn = getExport('wasm_fx_mutate_param_f32');
                if (fn) fn(instanceSlot >>> 0, regIdx >>> 0, Number(val));
            },
            mutateFxBit(instanceSlot, bitIdx, val) {
                const fn = getExport('wasm_fx_mutate_bit');
                if (fn) fn(instanceSlot >>> 0, bitIdx >>> 0, val ? 1 : 0);
            },
            invalidateTextCaches() {
                const fn = getOptionalExport('wasm_renderer_invalidate_text_caches');
                if (fn) fn();
            },
            readItemPresentation(slot, out) {
                ensureValidMemory();
                if (!out || typeof out !== 'object') {
                    throw new TypeError('[WasmBridge] Item presentation output must be an object.');
                }
                const ptr = getExport('wasm_pos_get_item_presentation_ptr')(slot >>> 0);
                const word = ptr >>> 2;
                out.tier = STATE.views.u32[word];
                out.maskCircle = (STATE.views.u32[word + 1] & 2) !== 0;
                out.maskHole = STATE.views.f32[word + 2];
                out.rotate = STATE.views.f32[word + 3];
                out.spin = STATE.views.f32[word + 4];
                out.volume = STATE.views.f32[word + 5];
                const revealMode = STATE.views.u32[word + 6];
                if (revealMode > 2) {
                    throw new RangeError('[WasmBridge] Native text reveal mode is invalid.');
                }
                out.revealMode = revealMode === 1 ? 'add' : revealMode === 2 ? 'display' : 'none';
                const float64Word = ptr >>> 3;
                out.revealDuration = STATE.views.f64[float64Word + 4];
                out.revealStagger = STATE.views.f64[float64Word + 5];
                if (!Number.isFinite(out.revealDuration) || out.revealDuration < 0 ||
                    !Number.isFinite(out.revealStagger) || out.revealStagger < 0) {
                    throw new RangeError('[WasmBridge] Native text reveal timing is invalid.');
                }
                return out;
            },
            putItem(slot, it, baseX = 0, baseY = 0) {
                const fn = getExport('wasm_pos_put_item');
                const typeMap = {
                    background: 1, video: 2, image: 3,
                    lyric: 5, text: 6, audio: 7, effect: 8
                };
                const t = typeMap[it.type] || 0;
                let flags = (it.spin ? 4 : 0);
                if (it.type === 'image' && it.maskHole > 0) flags |= 1;
                if (it.type === 'image' && it.maskCircle) flags |= 2;
                const spinSpeed = Number(it.spin) || 0;
                const vol = it.volume !== undefined ? Number(it.volume) : (it.volunm !== undefined ? Number(it.volunm) : (it.vol !== undefined ? Number(it.vol) : 1));
                const z = it.z || 0;
                const rotateDeg = Number(it.rotateDeg || it.rotation || 0);
                let resId = 0;
                if (it.type === 'background') {
                    const bgCol = (it.css && (it.css['background-color'] || it.css.background)) || it['background-color'] || it.background || '#000000';
                    resId = WasmBridge.manager.parseTimelineColor(bgCol, 0xFF000000);
                } else {
                    resId = strHash(it.src || it.id || '');
                }
                const tier = NATIVE_TIER[(Number(it.tier) || 0) & 3];
                fn(
                    slot >>> 0,
                    (it.uidNum || slot) >>> 0,
                    t >>> 0,
                    tier >>> 0,
                    flags >>> 0,
                    BigInt(Math.round(it.start || 0)),
                    BigInt(Math.round(it.end || 0)),
                    Number(it.width || it.baseW || ((global.state && global.state.renderConfig && global.state.renderConfig.baseW) || 1920)),
                    Number(it.height || it.baseH || ((global.state && global.state.renderConfig && global.state.renderConfig.baseH) || 1080)),
                    spinSpeed,
                    Number(it.maskHole) || 0,
                    vol,
                    z | 0,
                    resId >>> 0,
                    Number(baseX) || 0,
                    Number(baseY) || 0,
                    rotateDeg
                );
            },
            setKeyframe(slot, kfIdx, offsetMs, opacity = 1.0, x = 0, y = 0, w = 0, h = 0, rotateDeg = 0, scale = 1.0) {
                getExport('wasm_pos_set_keyframe')(slot >>> 0, kfIdx >>> 0, offsetMs | 0, opacity, x, y, w, h, rotateDeg, scale);
            },
            setTextParams(slot, params = {}) {
                getExport('wasm_pos_set_text_params')(
                    slot >>> 0,
                    Number(params.fontSize) || 32,
                    Number(params.lineHeight) || 1.25,
                    Number(params.letterSpacing) || 0,
                    Number(params.strokeWidth) || 0,
                    Number(params.anchorX) || 0,
                    Number(params.anchorY) || 0,
                    (params.color !== undefined ? params.color : 0xFFFFFFFF) >>> 0,
                    (params.strokeColor || 0) >>> 0,
                    (params.shadowColor || 0) >>> 0,
                    Number(params.shadowBlur) || 0,
                    Number(params.shadowOffsetX) || 0,
                    Number(params.shadowOffsetY) || 0,
                    (Number(params.fontWeight) || 400) >>> 0,
                    (Number(params.textAlign) || 0) >>> 0,
                    (Number(params.fontCategory) || 0) >>> 0,
                    (Number(params.revealDurationMs) || 0) >>> 0,
                    (Number(params.revealMode) || 0) >>> 0,
                    (Number(params.textTransform) || 0) >>> 0,
                    (Number(params.fontHash) || 0) >>> 0,
                    (Number(params.lineCount) || 1) >>> 0,
                    // styleBits: bit0 italic | bit1-3 decoration (underline, strike, superscript) | bit4 inheritColor
                    (((params.isItalic ? 1 : 0)) |
                        (((Number(params.textDecoration) || 0) & 7) << 1) |
                        ((params.inheritColor ? 1 : 0) << 4)) >>> 0
                );
            },
            getTextParamsPtr() {
                return getExport('wasm_pos_get_text_params_ptr')();
            },
            getPathParamsPtr() {
                const fn = getOptionalExport('wasm_pos_get_path_params_ptr');
                return fn ? fn() : 0;
            },
            getPathParams(slot) {
                const ptr = this.getPathParamsPtr();
                if (!ptr || !STATE.memory) return null;
                const slotOffset = ptr + (slot >>> 0) * 112;
                if (slotOffset + 112 > STATE.memory.buffer.byteLength) return null;
                const bitsLow = new Uint32Array(STATE.memory.buffer, slotOffset, 2)[0];
                const regF32 = new Float32Array(STATE.memory.buffer, slotOffset + 16, 24);
                const regU32 = new Uint32Array(STATE.memory.buffer, slotOffset + 16, 24);
                const hasFill = (bitsLow & (1 << 6)) !== 0 ? 1 : 0;
                const hasStroke = (bitsLow & (1 << 7)) !== 0 ? 1 : 0;
                const inheritFill = (bitsLow & (1 << 8)) !== 0 ? 1 : 0;
                const fillColor = regU32[14];
                const strokeColor = regU32[15];
                const strokeWidth = regF32[9];
                const strokeMiterlimit = regF32[10] > 0 ? regF32[10] : 4.0;
                const strokeDashoffset = regF32[11];
                const fillRule = ((bitsLow >> 1) & 1) === 1 ? 'evenodd' : 'nonzero';
                const lineCapVal = (bitsLow >> 2) & 3;
                const lineCap = lineCapVal === 1 ? 'round' : (lineCapVal === 2 ? 'square' : 'butt');
                const lineJoinVal = (bitsLow >> 4) & 3;
                const lineJoin = lineJoinVal === 1 ? 'round' : (lineJoinVal === 2 ? 'bevel' : 'miter');
                const toCss = (c) => {
                    const r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff, a = ((c >> 24) & 0xff) / 255;
                    return `rgba(${r}, ${g}, ${b}, ${a.toFixed(3)})`;
                };
                return {
                    fillColor, strokeColor, strokeWidth, strokeMiterlimit, strokeDashoffset,
                    fillRule, lineCap, lineJoin, hasFill, hasStroke, inheritFill,
                    fillCss: hasFill ? toCss(fillColor) : 'none',
                    strokeCss: hasStroke ? toCss(strokeColor) : 'none',
                    viewBox: [regF32[19], regF32[20], regF32[21], regF32[22]]
                };
            },
            setTrackInfo(slot, count, easing = 0, contentMask = 0, hasSpatial = 1) {
                getExport('wasm_pos_set_track_info')(slot >>> 0, count >>> 0, easing >>> 0, contentMask >>> 0, hasSpatial >>> 0);
            },
            getTrackCount(slot) {
                const fn = getOptionalExport('wasm_pos_get_track_count');
                return fn ? fn(slot >>> 0) : 0;
            },
            getTrackContentMask(slot) {
                const fn = getOptionalExport('wasm_pos_get_track_content_mask');
                return fn ? fn(slot >>> 0) : 0;
            },
            getTrackFrameOffset(slot, kfIdx) {
                const fn = getOptionalExport('wasm_pos_get_track_frame_offset');
                return fn ? fn(slot >>> 0, kfIdx >>> 0) : -1;
            },
            getTrackActiveSteps(slot, nowMs) {
                const fn = getOptionalExport('wasm_pos_get_track_active_steps');
                return fn ? fn(slot >>> 0, BigInt(Math.floor(nowMs))) : 0;
            },
            getSlotMode(slot) {
                const fn = getOptionalExport('wasm_pos_get_slot_mode');
                return fn ? fn(slot >>> 0) : -1;
            },
            getByModeBB(mode) {
                const fn = getOptionalExport('wasm_pos_get_by_mode_bb');
                return fn ? BigInt(fn(mode >>> 0)) : 0n;
            },
            getStateDirtyBB() {
                const fn = getOptionalExport('wasm_pos_get_state_dirty_bb');
                return fn ? BigInt(fn()) : 0n;
            },
            debugEval(slot, nowMs) {
                const fn = getOptionalExport('wasm_pos_debug_eval');
                return fn ? fn(slot >>> 0, BigInt(Math.floor(nowMs))) : 0;
            },
            debugStepInfo(slot) {
                const fn = getOptionalExport('wasm_pos_debug_step_info');
                return fn ? fn(slot >>> 0) : 0;
            },
            debugSlotFilter(slot) {
                const fn = getOptionalExport('wasm_pos_debug_slot_filter');
                return fn ? fn(slot >>> 0) : 0;
            },
            debugSlotFlags(slot) {
                const fn = getOptionalExport('wasm_pos_debug_slot_flags');
                return fn ? fn(slot >>> 0) : 0;
            },
            removeItem(slot) {
                getExport('wasm_pos_remove_item')(slot >>> 0);
            },
            setDuration(durMs) {
                const d = Number.isFinite(durMs) ? durMs : 0;
                getExport('wasm_pos_set_duration')(BigInt(Math.round(d)));
            },
            stepTo(nextTimeMs) {
                const t = Number.isFinite(nextTimeMs) ? nextTimeMs : 0;
                const c = getExport('wasm_pos_step_to')(BigInt(Math.floor(t)));
                dlCount = c;
                return c;
            },
            undoStep() {
                const c = getExport('wasm_pos_undo_step')();
                dlCount = c;
                return c;
            },
            getActiveMask() {
                return BigInt.asUintN(64, getExport('wasm_pos_get_active_mask')());
            },
            getActiveAudioMask() {
                return BigInt.asUintN(64, getExport('wasm_pos_get_active_audio_mask')());
            },
            getActiveMediaMask() {
                const fn = getExport('wasm_pos_get_active_media_mask');
                return fn ? BigInt.asUintN(64, fn()) : this.getActiveAudioMask();
            },
            setStageDimensions(w, h) {
                const rc = (global.state && global.state.renderConfig) || null;
                const defW = (rc && rc.baseW) || 1920;
                const defH = (rc && rc.baseH) || 1080;
                getExport('wasm_pos_set_stage_dimensions')(Number(w) || defW, Number(h) || defH);
            },
            getTime() {
                return Number(getExport('wasm_pos_get_time')());
            },
            hasItems() {
                return getExport('wasm_pos_has_items')() === 1;
            },
            setBassParams(slot, hop, colorCount, isBassActive) {
                getExport('wasm_pos_set_bass_params')(
                    slot >>> 0,
                    Number(hop) || 0.05,
                    colorCount >>> 0,
                    isBassActive ? 1 : 0
                );
            },
            setBassColor(slot, colorIdx, bgRgba, textRgba) {
                getExport('wasm_pos_set_bass_color')(
                    slot >>> 0,
                    colorIdx >>> 0,
                    bgRgba >>> 0,
                    textRgba >>> 0
                );
            },
            setBassHits(slot, hitsArray) {
                if (!hitsArray || !hitsArray.length) return;
                ensureValidMemory();
                const n = Math.min(8192, hitsArray.length);
                const inputPtr = STATE.textInputBufPtr;
                if (!inputPtr) return;
                const u16View = new Uint16Array(STATE.memory.buffer, inputPtr, n);
                for (let i = 0; i < n; i++) {
                    u16View[i] = hitsArray[i] & 0xFFFF;
                }
                getExport('wasm_pos_set_bass_hits')(slot >>> 0, inputPtr, n);
            },
            getDefinedProps(slot) {
                const fn = getExport('wasm_pos_get_defined_props');
                return fn ? BigInt(fn(slot >>> 0)) : 0n;
            },
            getKeyframedProps(slot) {
                const fn = getExport('wasm_pos_get_keyframed_props');
                return fn ? BigInt(fn(slot >>> 0)) : 0n;
            },
            getStaticZobristKey(slot) {
                const fn = getExport('wasm_pos_get_static_zobrist_key');
                return fn ? BigInt(fn(slot >>> 0)) : 0n;
            },
            getDefinedPropsWord(slot, wordIdx) {
                const fn = getExport('wasm_pos_get_defined_props_word');
                return fn ? BigInt(fn(slot >>> 0, wordIdx >>> 0)) : 0n;
            },
            getKeyframedPropsWord(slot, wordIdx) {
                const fn = getExport('wasm_pos_get_keyframed_props_word');
                return fn ? BigInt(fn(slot >>> 0, wordIdx >>> 0)) : 0n;
            },
            getTotalDefinedCount() {
                const fn = getExport('wasm_pos_get_total_defined_count');
                return fn ? fn() : 0;
            },
            getTotalKeyframedCount() {
                const fn = getExport('wasm_pos_get_total_keyframed_count');
                return fn ? fn() : 0;
            },
            getStaticSlotCount() {
                const fn = getExport('wasm_pos_get_static_slot_count');
                return fn ? fn() : 0;
            }
        },
        // --------------------------------------------------------------------
        // 15. C++ NATIVE AUDIO CLOCK & ENVELOPE ENGINE (ZERO-GC / ZERO-CRACKLE)
        // --------------------------------------------------------------------
        audioEngine: {
            _syncResult: { actionMask: 0, targetSec: 0, targetVolume: 1, driftEma: 0 },
            scheduleVolumeEnvelope(param, slot, at) {
                ensureValidMemory();
                if (!STATE.audioVolumeEnvelopesPtr) {
                    throw new Error('[WasmBridge FATAL] Audio volume-envelope engine is unavailable.');
                }
                if (!Number.isInteger(slot) || slot < 0 || slot >= 64) {
                    throw new Error('[WasmBridge FATAL] Invalid native audio track slot: ' + slot);
                }
                const base = (STATE.audioVolumeEnvelopesPtr + slot * 144) >> 2;
                const initialVolume = STATE.views.f32[base];
                const count = STATE.views.u32[base + 1];
                param.setValueAtTime(initialVolume, at);
                for (let i = 0; i < count; i++) {
                    const point = base + 2 + i * 2;
                    const offsetMs = STATE.views.u32[point];
                    const volume = STATE.views.f32[point + 1];
                    param.linearRampToValueAtTime(volume, at + offsetMs / 1000);
                }
            },
            syncTrack(slot, timelineNowMs, mediaCurrentSec, wallNowMs, isPlaying, isPaused, isSeeking) {
                if (STATE.ready && STATE.audioSyncResultPtr) {
                    ensureValidMemory();
                    const fn = getExport('wasm_audio_sync_track');
                    if (fn) {
                        const mask = fn(
                            slot >>> 0,
                            Number(timelineNowMs) || 0,
                            Number(mediaCurrentSec) || 0,
                            Number(wallNowMs) || 0,
                            isPlaying ? 1 : 0,
                            isPaused ? 1 : 0,
                            isSeeking ? 1 : 0
                        ) >>> 0;
                        const baseIdx = STATE.audioSyncResultPtr >> 2;
                        const r = this._syncResult;
                        r.actionMask = mask;
                        r.targetSec = STATE.views.f32[baseIdx + 1];
                        r.targetVolume = STATE.views.f32[baseIdx + 2];
                        r.driftEma = STATE.views.f32[baseIdx + 3];
                        return r;
                    }
                }
                return null;
            }
        },
        // --------------------------------------------------------------------
        // 16. C++ MASTER ACTIVE CLOCK & SHARED STATE (ZERO-COPY THE LIVING CORE)
        // --------------------------------------------------------------------
        clock: {
            getViews: getSharedStateViews,
            getTime() {
                const v = getSharedStateViews();
                return v.f64[0];
            },
            getDuration() {
                const v = getSharedStateViews();
                return v.f64[1];
            },
            getState() {
                const v = getSharedStateViews();
                return v.u32[4]; // 0=Stopped, 1=Playing, 2=Paused
            },
            getActiveCount() {
                const v = getSharedStateViews();
                return v.u32[5];
            },
            getActiveMask() {
                const v = getSharedStateViews();
                return v.dataView.getBigUint64(24, true);
            },
            getFrameIndex() {
                const v = getSharedStateViews();
                return v.u32[8];
            },
            getCommandCount() {
                const v = getSharedStateViews();
                return v.u32[10];
            },
            getActiveBgColor() {
                const v = getSharedStateViews();
                return v.u32[15];
            },
            getActiveTextColor() {
                const v = getSharedStateViews();
                return v.u32[16];
            },
            getActiveBassHitIdx() {
                const v = getSharedStateViews();
                return v.u32[17];
            },
            play() {
                if (!STATE.ready) return;
                getExport('wasm_clock_play')();
                const v = getSharedStateViews();
                if (v) v.u32[4] = 1;
            },
            pause() {
                if (!STATE.ready) return;
                getExport('wasm_clock_pause')();
                const v = getSharedStateViews();
                if (v) {
                    v.u32[4] = 2;
                    v.u32[29] &= ~1;
                }
            },
            stop() {
                if (!STATE.ready) return;
                getExport('wasm_clock_stop')();
                const v = getSharedStateViews();
                if (v) {
                    v.u32[4] = 0;
                    v.f64[0] = 0.0;
                    v.u32[29] &= ~1;
                }
            },
            setRate(rate) {
                if (!STATE.ready) return;
                const r = Number(rate) || 1.0;
                getExport('wasm_clock_set_rate')(r);
                const v = getSharedStateViews();
                if (v) v.f32[9] = r;
            },
            setDuration(durMs) {
                if (!STATE.ready) return;
                const d = Number(durMs) || 0.0;
                getExport('wasm_clock_set_duration')(d);
                const v = getSharedStateViews();
                if (v) v.f64[1] = d;
            },
            seek(timeMs) {
                if (!STATE.ready) return 0;
                const fn = getExport('wasm_clock_seek');
                return fn(Number(timeMs) || 0.0);
            },
            tickPreview(deltaMs) {
                getExport('wasm_clock_tick_preview')(deltaMs);
            },
            tick(deltaMs) {
                if (!STATE.ready) return 0;
                const fn = getExport('wasm_clock_tick');
                return fn(Number(deltaMs) || 0.0);
            }
        },
        // --------------------------------------------------------------------
        // 17. SLOT STATE TRACKER (STOCKFISH ZERO-MAP DELTA TRACKER)
        // --------------------------------------------------------------------
        slotTracker: {
            reset() {
                const fn = getExport('wasm_slot_tracker_reset');
                if (fn) fn();
            },
            evaluate(frameTick, stageW, stageH, maxDirty = 8, outDirtyBuf = null) {
                const fn = getExport('wasm_slot_tracker_evaluate');
                const count = fn ? fn(frameTick, stageW, stageH, maxDirty) : 0;
                if (count > 0 && outDirtyBuf) {
                    ensureValidMemory();
                    const f64Offset = STATE.dirtyBufPtr >> 3;
                    outDirtyBuf.set(STATE.views.f64.subarray(f64Offset, f64Offset + (count << 2)));
                }
                return count;
            },
            getStillN(slot, subSlot = 0) {
                const fn = getExport('wasm_slot_tracker_get_still_n');
                return fn ? fn(slot, subSlot) : 0;
            },
            getBounds(slot, subSlot = 0, out4) {
                const out = out4 || new Float32Array(4);
                const fn = getExport('wasm_slot_tracker_get_bounds');
                if (fn && STATE.areaPtr) {
                    fn(slot, subSlot, STATE.areaPtr);
                    out.set(STATE.views.f32.subarray(STATE.areaPtr >> 2, (STATE.areaPtr >> 2) + 4));
                }
                return out;
            }
        },
        // --------------------------------------------------------------------
        // 18. DIRECT VBO BATCH GENERATOR (PIXIJS v8 / OLIVE NLE VBO BUILDER)
        // --------------------------------------------------------------------
        vboBatcher: {
            getConstants() {
                if (!STATE.vboRenderConstants) {
                    ensureValidMemory();
                    const count = getExport('wasm_vbo_get_render_constants_count')();
                    const ptr = getExport('wasm_vbo_get_render_constants_ptr')();
                    const values = new Uint32Array(
                        STATE.currentBuf, ptr, count
                    );
                    STATE.vboRenderConstants = {
                        maxBatchQuads: values[0],
                        floatsPerVertex: values[1],
                        verticesPerQuad: values[2],
                        floatsPerQuad: values[3],
                        floatsPerQuadInput: values[4],
                        maxTextureSlots: values[5],
                        floatBytes: values[6],
                        vertexPositionComponents: values[7],
                        vertexUvComponents: values[8],
                        vertexColorComponents: values[9],
                        vertexTextureIdComponents: values[10],
                        vertexModeComponents: values[11],
                        vertexSpinComponents: values[12],
                        vertexPositionOffset: values[13],
                        vertexUvOffset: values[14],
                        vertexColorOffset: values[15],
                        vertexTextureIdOffset: values[16],
                        vertexModeOffset: values[17],
                        vertexSpinOffset: values[18],
                        maxDisplayCommands: values[19],
                        drawCommandWords: values[20],
                        drawCommandFloats: values[21],
                        renderCommandWords: values[22],
                        renderCommandFloats: values[23],
                        vboInputOffsets: Array.from(values.subarray(24, 42))
                    };
                }
                return STATE.vboRenderConstants;
            },
            reset() {
                const fn = getExport('wasm_vbo_reset');
                if (fn) fn();
            },
            getInputView() {
                ensureValidMemory();
                if (!STATE.vboInputBufferPtr) {
                    const fn = getExport('wasm_vbo_get_input_buffer_ptr');
                    if (!fn) {
                        throw new Error('[WasmBridge] Native VBO input buffer is unavailable.');
                    }
                    STATE.vboInputBufferPtr = fn();
                    STATE.vboInputView = new Float32Array(
                        STATE.currentBuf,
                        STATE.vboInputBufferPtr,
                        this.getConstants().maxBatchQuads *
                        this.getConstants().floatsPerQuadInput
                    );
                }
                return STATE.vboInputView;
            },
            pushBatch(count) {
                if (!Number.isInteger(count) || count < 0 ||
                    count > this.getConstants().maxBatchQuads) {
                    throw new RangeError('[WasmBridge] Invalid VBO batch size.');
                }
                const fn = getExport('wasm_vbo_push_batch');
                if (!fn) {
                    throw new Error('[WasmBridge] Native VBO batch builder is unavailable.');
                }
                return fn(count);
            },
            getBufferPtr() {
                const fn = getExport('wasm_vbo_get_buffer_ptr');
                return fn ? fn() : 0;
            },
            getQuadCount() {
                const fn = getExport('wasm_vbo_get_quad_count');
                return fn ? fn() : 0;
            },
            getView() {
                ensureValidMemory();
                const ptr = this.getBufferPtr();
                const count = this.getQuadCount();
                return STATE.views.f32.subarray(ptr >> 2, (ptr >> 2) + count * 66);
            }
        },
        // --------------------------------------------------------------------
        // 19. NATRON EFFECT ENGINE & PHYSICAL TRANSITIONS (C++ SIMD / ZERO-COPY)
        // --------------------------------------------------------------------
        effectEngine: {
            computeIntroOutroState(isOutro, u) {
                const fn = getExport('wasm_compute_intro_outro_state');
                if (!fn) return false;
                const active = fn(isOutro ? 1 : 0, u) === 1;
                forwardEffectCacheMiss();
                return active;
            },
            getStatePtr() {
                const fn = getExport('wasm_get_intro_outro_state_buf');
                return fn ? fn() : 0;
            },
            getStateView() {
                ensureValidMemory();
                const ptr = this.getStatePtr();
                return STATE.views.f32.subarray(ptr >> 2, (ptr >> 2) + 38);
            },
            isIdentity(effectType, nowMs, startMs, endMs, param0 = 0, param1 = 0) {
                const fn = getExport('wasm_effect_is_identity');
                return fn ? (fn(effectType, nowMs, startMs, endMs, param0, param1) === 1) : false;
            },
            computeRoI(effectType, nowMs, startMs, endMs, W, H, inX0, inY0, inX1, inY1, outRoI4) {
                const fn = getExport('wasm_effect_compute_roi');
                if (fn && STATE.effectScratchPtr) {
                    fn(effectType, nowMs, startMs, endMs, W, H, inX0, inY0, inX1, inY1, STATE.effectScratchPtr);
                    ensureValidMemory();
                    const off = STATE.effectScratchPtr >> 2;
                    if (outRoI4) {
                        outRoI4[0] = STATE.views.f32[off];
                        outRoI4[1] = STATE.views.f32[off + 1];
                        outRoI4[2] = STATE.views.f32[off + 2];
                        outRoI4[3] = STATE.views.f32[off + 3];
                    }
                    forwardEffectCacheMiss();
                    return outRoI4;
                }
                return null;
            }
        },
        // --------------------------------------------------------------------
        // 20. NATRON PROXY RENDER SCALE (SCRUBBING & DOWNSAMPLING SUBSYSTEM)
        // --------------------------------------------------------------------
        renderScale: {
            set(sx, sy) {
                const fn = getExport('wasm_render_scale_set');
                if (fn) fn(sx, sy);
            },
            setLevel(level) {
                const fn = getExport('wasm_render_scale_set_level');
                if (fn) fn(level);
            },
            get(outScale2) {
                const fn = getExport('wasm_render_scale_get');
                if (fn && STATE.areaPtr) {
                    fn(STATE.areaPtr);
                    ensureValidMemory();
                    const off = STATE.areaPtr >> 2;
                    const sx = STATE.views.f32[off];
                    const sy = STATE.views.f32[off + 1];
                    if (outScale2) {
                        outScale2[0] = sx;
                        outScale2[1] = sy;
                    }
                    return [sx, sy];
                }
                return [1.0, 1.0];
            },
            isIdentity() {
                const fn = getExport('wasm_render_scale_is_identity');
                return fn ? (fn() === 1) : true;
            }
        },
        // --------------------------------------------------------------------
        // 21. HEAVY MATHEMATICAL EFFECT SIMULATION ENGINES (C++ SIMD NATIVE)
        // --------------------------------------------------------------------
        effectMath: {
            zoomEvaluatePeriodic(speed, amplitude, phase, tilt, mirror, tMs, out3) {
                const fn = getExport('wasm_zoom_evaluate_periodic');
                if (!fn || !STATE.effectScratchPtr) {
                    throw new Error('[WasmBridge FATAL] Missing native zoom evaluator');
                }
                fn(speed, amplitude, phase, tilt, mirror ? 1 : 0, tMs, STATE.effectScratchPtr);
                ensureValidMemory();
                const off = STATE.effectScratchPtr >> 2;
                const result = out3 || effectZoomResult;
                result[0] = STATE.views.f32[off];
                result[1] = STATE.views.f32[off + 1];
                result[2] = STATE.views.f32[off + 2] > 0.5 ? 1 : 0;
                forwardEffectCacheMiss();
                return result;
            },
            shakeEvaluate(speed, amplitude, tilt, fadeMs, tMs, durMs, W, H, out4) {
                const fn = getExport('wasm_shake_evaluate');
                if (!fn || !STATE.effectScratchPtr) {
                    throw new Error('[WasmBridge FATAL] Missing native shake evaluator');
                }
                fn(speed, amplitude, tilt, fadeMs, tMs, durMs, W, H, STATE.effectScratchPtr);
                ensureValidMemory();
                const off = STATE.effectScratchPtr >> 2;
                const result = out4 || effectShakeResult;
                result[0] = STATE.views.f32[off];
                result[1] = STATE.views.f32[off + 1];
                result[2] = STATE.views.f32[off + 2];
                result[3] = STATE.views.f32[off + 3];
                forwardEffectCacheMiss();
                return result;
            },
            shakeScaleBoost(amplitude, tiltDeg, W, H) {
                const fn = getExport('wasm_shake_scale_boost');
                if (!fn) {
                    throw new Error('[WasmBridge FATAL] Missing native shake scale evaluator');
                }
                return fn(amplitude, tiltDeg, W, H);
            },
            flickerEvaluatePeriodic(speed, brightness, baseVal, isSquare, tMs) {
                const fn = getExport('wasm_flicker_evaluate_periodic');
                if (!fn) {
                    throw new Error('[WasmBridge FATAL] Missing native flicker evaluator');
                }
                const result = fn(speed, brightness, baseVal, isSquare ? 1 : 0, tMs);
                forwardEffectCacheMiss();
                return result;
            },
            renderIntroField(tau, m, flip, ringR, barY0, barY1, barPIn, barPOut, barTIn, barTOut, barSp, barActive, lineYc, linePlateau, lineCore, lineHw1, lineHw2, lineState, lineActive, lw, lh, W, H, baseW, baseH, outPixelsU32) {
                const fn = getExport('wasm_effect_render_intro_field');
                if (fn && outPixelsU32) {
                    ensureValidMemory();
                    // ÄÆ°a bá»™ Ä‘á»‡m ImageData vÃ o tháº³ng Linear Memory hoáº·c render qua static scratch
                    const scratchPtr = STATE.displayListPtr || STATE.areaPtr;
                    const rendered = fn(
                        tau, m, flip ? 1 : 0,
                        ringR,
                        barY0, barY1, barPIn, barPOut, barTIn, barTOut, barSp, barActive ? 1 : 0,
                        lineYc, linePlateau, lineCore, lineHw1, lineHw2, lineState, lineActive ? 1 : 0,
                        lw, lh,
                        W, H,
                        baseW, baseH,
                        scratchPtr
                    );
                    const count = lw * lh;
                    const u32Off = scratchPtr >> 2;
                    outPixelsU32.set(STATE.views.u32.subarray(u32Off, u32Off + count));
                    return rendered;
                }
                return 0;
            }
        },
        // --------------------------------------------------------------------
        // 22. OBS STUDIO 4x4 SIMD MATRIX PIPELINE & STACK
        // --------------------------------------------------------------------
        matrix: {
            reset() {
                const fn = getExport('wasm_matrix_stack_reset');
                if (fn) fn();
            },
            push() {
                const fn = getExport('wasm_matrix_stack_push');
                if (fn) fn();
            },
            pop() {
                const fn = getExport('wasm_matrix_stack_pop');
                if (fn) fn();
            },
            translate(tx, ty, tz = 0.0) {
                const fn = getExport('wasm_matrix_stack_translate');
                if (fn) fn(tx, ty, tz);
            },
            scale(sx, sy, sz = 1.0) {
                const fn = getExport('wasm_matrix_stack_scale');
                if (fn) fn(sx, sy, sz);
            },
            rotateZ(rad) {
                const fn = getExport('wasm_matrix_stack_rotate_z');
                if (fn) fn(rad);
            },
            getCurrent(out16) {
                const fn = getExport('wasm_matrix_stack_get_current');
                if (fn) {
                    const ptr = fn();
                    ensureValidMemory();
                    const off = ptr >> 2;
                    const view = STATE.views.f32.subarray(off, off + 16);
                    if (out16) {
                        out16.set(view);
                        return out16;
                    }
                    return view;
                }
                return null;
            },
            ortho(left, right, bottom, top, zNear, zFar, out16) {
                const fn = getExport('wasm_matrix4_ortho');
                if (fn && STATE.areaPtr) {
                    fn(left, right, bottom, top, zNear, zFar, STATE.areaPtr);
                    ensureValidMemory();
                    const off = STATE.areaPtr >> 2;
                    const view = STATE.views.f32.subarray(off, off + 16);
                    if (out16) {
                        out16.set(view);
                        return out16;
                    }
                    return view;
                }
                return null;
            },
            perspective(fovYDeg, aspect, zNear, zFar, out16) {
                const fn = getExport('wasm_matrix4_perspective');
                if (fn && STATE.areaPtr) {
                    fn(fovYDeg, aspect, zNear, zFar, STATE.areaPtr);
                    ensureValidMemory();
                    const off = STATE.areaPtr >> 2;
                    const view = STATE.views.f32.subarray(off, off + 16);
                    if (out16) {
                        out16.set(view);
                        return out16;
                    }
                    return view;
                }
                return null;
            }
        },
        // --------------------------------------------------------------------
        // 23. NATRON REVERSE-DAG RoD / RoI OPTIMIZATION ENGINE
        // --------------------------------------------------------------------
        natron: {
            optimizeRenderPlan(effectType, reqX1, reqY1, reqX2, reqY2, rodX1, rodY1, rodX2, rodY2, nowMs, startMs, endMs, param0 = 0, param1 = 0, W = 1920, H = 1080) {
                const fn = getExport('wasm_natron_optimize_render_plan');
                if (fn) {
                    const planPtr = fn(
                        effectType,
                        reqX1, reqY1, reqX2, reqY2,
                        rodX1, rodY1, rodX2, rodY2,
                        nowMs, startMs, endMs,
                        param0, param1,
                        W, H
                    );
                    ensureValidMemory();
                    const f32Off = planPtr >> 2;
                    const u32Off = planPtr >> 2;
                    return {
                        renderRoI: [
                            STATE.views.f32[f32Off],
                            STATE.views.f32[f32Off + 1],
                            STATE.views.f32[f32Off + 2],
                            STATE.views.f32[f32Off + 3]
                        ],
                        isIdentity: STATE.views.u32[u32Off + 4] === 1,
                        isCulled: STATE.views.u32[u32Off + 5] === 1,
                        splitRectCount: STATE.views.u32[u32Off + 6]
                    };
                }
                return {
                    renderRoI: [reqX1, reqY1, reqX2, reqY2],
                    isIdentity: false,
                    isCulled: false,
                    splitRectCount: 1
                };
            }
        },
        // --------------------------------------------------------------------
        // 24. NATIVE OPENGL ES 3.0 / WEBGL2 DIRECT RENDERER (ZERO-JS RENDER)
        // --------------------------------------------------------------------
        nativeRenderer: {
            init() {
                const fn = getExport('wasm_render_init');
                return fn ? (fn() === 1) : false;
            },
            hasContext() {
                const fn = getExport('wasm_render_has_context');
                return fn ? (fn() === 1) : false;
            },
            allocTexture() {
                const fn = getExport('wasm_render_alloc_texture');
                return fn ? (fn() >>> 0) : 0;
            },
            bindTexture(texId) {
                const fn = getExport('wasm_render_bind_texture');
                if (fn) fn(texId >>> 0);
            },
            freeTexture(texId) {
                const fn = getExport('wasm_render_free_texture');
                if (fn) fn(texId >>> 0);
            },
            registerStaticTexture(slot, texId, width, height) {
                const fn = getExport('wasm_fx_register_static_texture');
                if (fn) fn(slot >>> 0, texId >>> 0, width >>> 0, height >>> 0);
            },
            syncGlyphAtlas() {
                const fn = getExport('wasm_render_sync_glyph_atlas');
                if (fn) fn();
            },
            getGlyphAtlasTexId() {
                const fn = getExport('wasm_render_get_glyph_atlas_tex_id');
                return fn ? (fn() >>> 0) : 0;
            },
            getSceneFboId() {
                const fn = getExport('wasm_render_get_scene_fbo_id');
                return fn ? (fn() >>> 0) : 0;
            },
            getSceneTexId() {
                const fn = getExport('wasm_render_get_scene_tex_id');
                return fn ? (fn() >>> 0) : 0;
            },
            storeGlyphCache(cacheSlot, glyphDataF32, glyphCount, lineWidthsF32, lineCount, lineHeight, alignCode, baselineOffset) {
                const fn = getExport('wasm_render_store_glyph_cache');
                const gPtrFn = getExport('wasm_render_get_glyph_upload_scratch');
                const lPtrFn = getExport('wasm_render_get_line_widths_scratch');
                if (!fn || !gPtrFn || !lPtrFn) return;
                ensureValidMemory();
                const gPtr = gPtrFn() >>> 0;
                const lPtr = lPtrFn() >>> 0;
                const cappedGlyphs = Math.min(512, glyphCount >>> 0);
                const cappedLines = Math.min(32, lineCount >>> 0);
                if (cappedGlyphs > 0 && glyphDataF32) {
                    STATE.views.f32.set(glyphDataF32.subarray(0, cappedGlyphs * 8), gPtr >> 2);
                }
                if (cappedLines > 0 && lineWidthsF32) {
                    STATE.views.f32.set(lineWidthsF32.subarray(0, cappedLines), lPtr >> 2);
                }
                fn(
                    cacheSlot >>> 0,
                    cappedGlyphs,
                    cappedLines,
                    lineHeight,
                    alignCode >>> 0,
                    baselineOffset
                );
            },
            executeFrame(width, height, compositeMode, useNativeRenderPlan, commandCount, staticPrefixCount, staticKey, dirtyRectCount, bgR, bgG, bgB, bgA, blitToScreen = 1) {
                const fn = getExport('wasm_render_execute_frame');
                if (!fn) {
                    throw new Error('[WasmBridge FATAL] Missing wasm_render_execute_frame export');
                }
                return fn(
                    width >>> 0,
                    height >>> 0,
                    compositeMode >>> 0,
                    useNativeRenderPlan ? 1 : 0,
                    commandCount >>> 0,
                    staticPrefixCount >>> 0,
                    staticKey >>> 0,
                    dirtyRectCount >>> 0,
                    bgR, bgG, bgB, bgA,
                    blitToScreen ? 1 : 0
                ) >>> 0;
            },
            renderTimelineFrame(nowMs, width, height, frameTick, sceneValid, bgR = 0, bgG = 0, bgB = 0, bgA = 1, blitToScreen = 1) {
                const fn = getExport('wasm_render_timeline_frame');
                if (!fn) throw new Error('[WasmBridge] wasm_render_timeline_frame is not available.');
                return fn(
                    Number(nowMs),
                    width >>> 0,
                    height >>> 0,
                    frameTick >>> 0,
                    sceneValid ? 1 : 0,
                    Number(bgR),
                    Number(bgG),
                    Number(bgB),
                    Number(bgA),
                    blitToScreen ? 1 : 0
                ) >>> 0;
            },
            setSlotTexture(slot, texId) {
                const fn = getExport('wasm_render_set_slot_texture');
                if (fn) fn(slot >>> 0, texId >>> 0);
            },
            getSlotTexturesView() {
                const fn = getOptionalExport('wasm_render_get_slot_textures_ptr');
                if (!fn) return null;
                try {
                    const ptr = fn();
                    if (!ptr) return null;
                    const buf = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU32 && STATE.instance.HEAPU32.buffer);
                    if (!buf) return null;
                    return new Uint32Array(buf, ptr, 64);
                } catch (e) {
                    return null;
                }
            },
            setResourceTexture(resourceId, texId) {
                const fn = getExport('wasm_render_set_resource_texture');
                if (fn) fn(resourceId >>> 0, texId >>> 0);
            },
            computeResourceId(str) {
                if (typeof str !== 'string' || !str.length) return 0;
                let hash = 0;
                for (let i = 0; i < str.length; i++) {
                    hash = (((hash << 5) - hash) + (str.charCodeAt(i) & 0xFF)) >>> 0;
                }
                return hash;
            },
            setSlotGlyphCache(slot, cacheSlot) {
                const fn = getExport('wasm_render_set_slot_glyph_cache');
                if (fn) fn(slot >>> 0, cacheSlot >>> 0);
            },
            getGlyphCacheCount(cacheSlot) {
                const fn = getExport('wasm_render_get_glyph_cache_count');
                return fn ? (fn(cacheSlot >>> 0) >>> 0) : 0;
            },
            getProjectDuration() {
                return _projectDurationResult.durationMs || 0;
            },
            resetProjectState() {
                const fn = getExport('wasm_render_reset_project_state');
                if (fn) fn();
            },
            getStateTransferPolicy() {
                const fn = getExport('wasm_shared_get_state_transfer_policy');
                return fn ? (fn() >>> 0) : 0;
            },
            batchEvaluateTimeline(startMs, endMs, stepMs, maxFrames = 16384) {
                const fn = getOptionalExport('wasm_render_batch_evaluate');
                const getPtr = getOptionalExport('wasm_render_get_batch_buffer_ptr');
                if (!fn || !getPtr) return null;
                const count = fn(Number(startMs), Number(endMs), Number(stepMs), maxFrames >>> 0) >>> 0;
                if (count === 0) return new BigUint64Array(0);
                const ptr = getPtr();
                const buf = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU32 && STATE.instance.HEAPU32.buffer);
                if (!buf) return null;
                return new BigUint64Array(buf.slice(ptr, ptr + count * 8));
            }
        },
        get nativeRender() {
            return this.nativeRenderer;
        },
        stream: (() => {
            const _sources = new Map();
            const _inFlight = new Set();
            function _calcResId(str) {
                if (typeof str !== 'string' || !str.length) return 0;
                let hash = 0;
                for (let i = 0; i < str.length; i++) {
                    hash = (((hash << 5) - hash) + (str.charCodeAt(i) & 0xFF)) >>> 0;
                }
                return hash;
            }
            return {
                registerSource(srcPath, descriptor = {}) {
                    if (!srcPath) return 0;
                    const resId = (WasmBridge.nativeRenderer && typeof WasmBridge.nativeRenderer.computeResourceId === 'function')
                        ? WasmBridge.nativeRenderer.computeResourceId(String(srcPath))
                        : _calcResId(String(srcPath));
                    if (!resId) return 0;
                    const prev = _sources.get(resId) || {};
                    const merged = {
                        src: srcPath,
                        handle: descriptor.handle || prev.handle || null,
                        file: descriptor.file || prev.file || null,
                        url: descriptor.url || prev.url || (typeof srcPath === 'string' ? srcPath : null),
                        mediaType: descriptor.mediaType || prev.mediaType || 0,
                        size: descriptor.size || (descriptor.file && descriptor.file.size) || prev.size || 0
                    };
                    _sources.set(resId, merged);
                    if (STATE.ready) {
                        const regFn = getOptionalExport('wasm_stream_register_resource');
                        if (regFn) regFn(resId >>> 0, merged.mediaType >>> 0, Number(merged.size) || 0);
                        this.pumpRequests();
                    }
                    return resId;
                },
                scheduleAt(timeMs, isPlaying = false) {
                    if (!STATE.ready) return;
                    const schedFn = getOptionalExport('wasm_stream_schedule_at');
                    if (!schedFn) return;
                    const count = schedFn(Number(timeMs) || 0, isPlaying ? 1 : 0) >>> 0;
                    if (count > 0) this.pumpRequests();
                },
                getDurationMs(srcOrResId) {
                    if (!STATE.ready) return 0;
                    const resId = typeof srcOrResId === 'number'
                        ? (srcOrResId >>> 0)
                        : ((WasmBridge.nativeRenderer && typeof WasmBridge.nativeRenderer.computeResourceId === 'function')
                            ? WasmBridge.nativeRenderer.computeResourceId(String(srcOrResId || ''))
                            : _calcResId(String(srcOrResId || '')));
                    const fn = getOptionalExport('wasm_stream_get_resource_duration_ms');
                    return (fn && resId) ? (fn(resId) >>> 0) : 0;
                },
                pumpRequests() {
                    if (!STATE.ready) return;
                    const countFn = getOptionalExport('wasm_stream_get_request_count');
                    const ptrFn = getOptionalExport('wasm_stream_get_requests_ptr');
                    const clearFn = getOptionalExport('wasm_stream_clear_requests');
                    const commitFn = getOptionalExport('wasm_stream_commit_chunk');
                    if (!countFn || !ptrFn || !clearFn || !commitFn) return;
                    ensureValidMemory();
                    const count = countFn() >>> 0;
                    if (!count) return;
                    const basePtr = ptrFn() >>> 0;
                    const cmds = [];
                    for (let i = 0; i < count; i++) {
                        const byteOff = basePtr + i * 32;
                        const u32Idx = byteOff >> 2;
                        const f64Idx = (byteOff + 16) >> 3;
                        cmds.push({
                            resourceId: STATE.views.u32[u32Idx] >>> 0,
                            slot: STATE.views.u32[u32Idx + 1] >>> 0,
                            ringPageIdx: STATE.views.u32[u32Idx + 2] >>> 0,
                            wasmDestPtr: STATE.views.u32[u32Idx + 3] >>> 0,
                            byteStart: STATE.views.f64[f64Idx],
                            byteLength: STATE.views.u32[u32Idx + 6] >>> 0,
                            flags: STATE.views.u32[u32Idx + 7] >>> 0
                        });
                    }
                    clearFn();
                    for (const cmd of cmds) {
                        const key = `${cmd.resourceId}:${cmd.byteStart}`;
                        if (_inFlight.has(key)) continue;
                        const info = _sources.get(cmd.resourceId);
                        if (!info) continue;
                        _inFlight.add(key);
                        (async () => {
                            try {
                                let ab = null;
                                if (info.file || info.handle) {
                                    const f = info.file || await info.handle.getFile();
                                    if (f) {
                                        info.file = f;
                                        info.size = f.size;
                                        const end = Math.min(f.size, cmd.byteStart + cmd.byteLength);
                                        if (cmd.byteStart < end) {
                                            ab = await f.slice(cmd.byteStart, end).arrayBuffer();
                                        }
                                    }
                                } else if (info.url) {
                                    const endByte = cmd.byteStart + cmd.byteLength - 1;
                                    const resp = await fetch(info.url, {
                                        headers: { 'Range': `bytes=${cmd.byteStart}-${endByte}` }
                                    });
                                    if (resp.ok || resp.status === 206) {
                                        ab = await resp.arrayBuffer();
                                    }
                                }
                                if (ab && ab.byteLength > 0) {
                                    ensureValidMemory();
                                    const validLen = Math.min(ab.byteLength, cmd.byteLength);
                                    if (cmd.wasmDestPtr + validLen <= STATE.views.u8.length) {
                                        STATE.views.u8.set(new Uint8Array(ab, 0, validLen), cmd.wasmDestPtr);
                                        commitFn(cmd.resourceId, cmd.ringPageIdx, cmd.byteStart, validLen);
                                    }
                                }
                            } catch (_) {
                            } finally {
                                _inFlight.delete(key);
                            }
                        })();
                    }
                }
            };
        })(),
        resources: {
            register(mediaType, path, name = '', title = '', artist = '', genre = '', fileSize = 0, durationMs = 0, width = 0, height = 0, flags = 0) {
                const fn = getOptionalExport('wasm_res_register');
                const getBuf = getOptionalExport('wasm_res_get_input_buffer_ptr');
                if (!fn || !getBuf) return 0;
                const bufPtr = getBuf();
                const mem = new Uint8Array((STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU8 && STATE.instance.HEAPU8.buffer));
                const encoder = new TextEncoder();
                let offset = bufPtr;

                const pBytes = encoder.encode(path || '');
                mem.set(pBytes, offset);
                mem[offset + pBytes.length] = 0;
                const pathPtr = offset;
                offset += pBytes.length + 1;

                const nBytes = encoder.encode(name || '');
                mem.set(nBytes, offset);
                mem[offset + nBytes.length] = 0;
                const namePtr = offset;
                offset += nBytes.length + 1;

                const tBytes = encoder.encode(title || '');
                mem.set(tBytes, offset);
                mem[offset + tBytes.length] = 0;
                const titlePtr = offset;
                offset += tBytes.length + 1;

                const aBytes = encoder.encode(artist || '');
                mem.set(aBytes, offset);
                mem[offset + aBytes.length] = 0;
                const artistPtr = offset;
                offset += aBytes.length + 1;

                const gBytes = encoder.encode(genre || '');
                mem.set(gBytes, offset);
                mem[offset + gBytes.length] = 0;
                const genrePtr = offset;

                return fn(
                    mediaType >>> 0,
                    pathPtr,
                    namePtr,
                    titlePtr,
                    artistPtr,
                    genrePtr,
                    Number(fileSize) || 0,
                    durationMs >>> 0,
                    width >>> 0,
                    height >>> 0,
                    flags >>> 0
                ) >>> 0;
            },
            unregister(resourceId) {
                const fn = getOptionalExport('wasm_res_unregister');
                return fn ? (fn(resourceId >>> 0) === 1) : false;
            },
            getCount() {
                const fn = getOptionalExport('wasm_res_get_count');
                return fn ? (fn() >>> 0) : 0;
            },
            getCountByType(mediaType) {
                const fn = getOptionalExport('wasm_res_get_count_by_type');
                return fn ? (fn(mediaType >>> 0) >>> 0) : 0;
            },
            getIdByTypeIndex(mediaType, index) {
                const fn = getOptionalExport('wasm_res_get_id_by_type_index');
                return fn ? (fn(mediaType >>> 0, index >>> 0) >>> 0) : 0;
            },
            findByPath(path) {
                const fn = getOptionalExport('wasm_res_find_by_path');
                const getBuf = getOptionalExport('wasm_res_get_input_buffer_ptr');
                if (!fn || !getBuf || !path) return 0;
                const bufPtr = getBuf();
                const mem = new Uint8Array((STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU8 && STATE.instance.HEAPU8.buffer));
                const pBytes = new TextEncoder().encode(path);
                mem.set(pBytes, bufPtr);
                mem[bufPtr + pBytes.length] = 0;
                return fn(bufPtr) >>> 0;
            },
            findByName(name) {
                const fn = getOptionalExport('wasm_res_find_by_name');
                const getBuf = getOptionalExport('wasm_res_get_input_buffer_ptr');
                if (!fn || !getBuf || !name) return 0;
                const bufPtr = getBuf();
                const mem = new Uint8Array((STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU8 && STATE.instance.HEAPU8.buffer));
                const nBytes = new TextEncoder().encode(name);
                mem.set(nBytes, bufPtr);
                mem[bufPtr + nBytes.length] = 0;
                return fn(bufPtr) >>> 0;
            },
            getEntry(resourceId) {
                const getPtr = getOptionalExport('wasm_res_get_entry_ptr');
                if (!getPtr) return null;
                const ptr = getPtr(resourceId >>> 0);
                if (!ptr) return null;
                const memBuf = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU8 && STATE.instance.HEAPU8.buffer);
                if (!memBuf) return null;

                const u8 = new Uint8Array(memBuf);
                const u32 = new Uint32Array(memBuf);

                function readStr(offset, maxLen) {
                    let end = offset;
                    const limit = offset + maxLen;
                    while (end < limit && u8[end] !== 0) end++;
                    return new TextDecoder().decode(u8.subarray(offset, end));
                }

                const rId = u32[(ptr + 0) >> 2] >>> 0;
                const mType = u32[(ptr + 4) >> 2] >>> 0;
                const fSize = Number(new BigUint64Array(memBuf, ptr + 16, 1)[0]);
                const durMs = u32[(ptr + 24) >> 2] >>> 0;
                const u16 = new Uint16Array(memBuf);
                const w = u16[(ptr + 32) >> 1];
                const h = u16[(ptr + 34) >> 1];
                const flags = u16[(ptr + 36) >> 1];
                const memBytes = u32[(ptr + 44) >> 2] >>> 0;

                const pathStr = readStr(ptr + 48, 256);
                const nameStr = readStr(ptr + 304, 64);
                const titleStr = readStr(ptr + 368, 64);
                const artistStr = readStr(ptr + 432, 64);
                const genreStr = readStr(ptr + 496, 32);

                const typeNames = ['video', 'image', 'audio', 'font'];
                const typeStr = typeNames[mType] || 'video';

                return {
                    resourceId: rId,
                    id: `res_${typeStr}_${nameStr.replace(/[^a-zA-Z0-9]/g, '_')}`,
                    key: pathStr,
                    path: pathStr,
                    name: nameStr,
                    title: titleStr || nameStr,
                    artist: artistStr,
                    genre: genreStr || 'All',
                    type: typeStr,
                    mediaType: mType,
                    size: fSize,
                    duration: durMs > 0 ? (durMs / 1000) : 0,
                    durationMs: durMs,
                    width: w,
                    height: h,
                    flags,
                    memoryBytes: memBytes,
                    isAsset: !!(flags & 1),
                    isProjectFile: !!(flags & 2),
                    isMemoryLoaded: !!(flags & 4)
                };
            },
            touch(resourceId) {
                const fn = getOptionalExport('wasm_res_touch');
                if (fn) fn(resourceId >>> 0);
            },
            setMemoryLoaded(resourceId, bytes) {
                const fn = getOptionalExport('wasm_res_set_memory_loaded');
                if (fn) fn(resourceId >>> 0, bytes >>> 0);
            },
            setMemoryUnloaded(resourceId) {
                const fn = getOptionalExport('wasm_res_set_memory_unloaded');
                if (fn) fn(resourceId >>> 0);
            },
            enforceBudget(maxBytes) {
                const fn = getOptionalExport('wasm_res_enforce_budget');
                return fn ? (fn(maxBytes >>> 0) >>> 0) : 0;
            },
            clearProjectFiles() {
                const fn = getOptionalExport('wasm_res_clear_project_files');
                return fn ? (fn() >>> 0) : 0;
            },
            pollEvent() {
                const fn = getOptionalExport('wasm_res_poll_event');
                const getBuf = getOptionalExport('wasm_res_get_input_buffer_ptr');
                if (!fn || !getBuf) return null;
                const bufPtr = getBuf();
                const mem = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU32 && STATE.instance.HEAPU32.buffer);
                if (!mem) return null;
                const u32 = new Uint32Array(mem);
                const typePtr = bufPtr;
                const idPtr = bufPtr + 4;
                const mTypePtr = bufPtr + 8;
                const ok = fn(typePtr, idPtr, mTypePtr);
                if (!ok) return null;
                return {
                    eventType: u32[typePtr >> 2] >>> 0,
                    resourceId: u32[idPtr >> 2] >>> 0,
                    mediaType: u32[mTypePtr >> 2] >>> 0
                };
            }
        },
        selection: {
            computeOutlines(topLevelIndices) {
                if (!STATE.ready) return 0;
                const compFn = getOptionalExport('wasm_selection_compute_outlines');
                if (!compFn) return 0;
                if (!topLevelIndices || topLevelIndices.length === 0) {
                    return compFn(0);
                }
                const getInPtr = getOptionalExport('wasm_selection_get_input_indices_ptr');
                if (!getInPtr) return 0;
                const inPtr = getInPtr();
                const mem = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU32 && STATE.instance.HEAPU32.buffer);
                if (!mem) return 0;
                const u32 = new Uint32Array(mem);
                const count = Math.min(topLevelIndices.length, 64);
                const baseIdx = inPtr >> 2;
                for (let i = 0; i < count; i++) {
                    u32[baseIdx + i] = topLevelIndices[i] >>> 0;
                }
                return compFn(count);
            },
            getSegments(count) {
                if (!STATE.ready || count <= 0) return null;
                const getSegPtr = getOptionalExport('wasm_selection_get_segments_ptr');
                if (!getSegPtr) return null;
                const segPtr = getSegPtr();
                const mem = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPF32 && STATE.instance.HEAPF32.buffer);
                if (!mem) return null;
                return new Float32Array(mem, segPtr, count * 4);
            },
            hitTest(stageX, stageY) {
                if (!STATE.ready) return -1;
                const fn = getOptionalExport('wasm_selection_hit_test');
                if (!fn) return -1;
                return fn(stageX, stageY);
            },
            marqueeHitTest(x0, y0, x1, y1) {
                if (!STATE.ready) return [];
                const fn = getOptionalExport('wasm_selection_marquee_hit_test');
                const getHitPtr = getOptionalExport('wasm_selection_get_hit_indices_ptr');
                if (!fn || !getHitPtr) return [];
                const count = fn(x0, y0, x1, y1);
                if (count <= 0) return [];
                const hitPtr = getHitPtr();
                const mem = (STATE.memory && STATE.memory.buffer) || (STATE.instance && STATE.instance.HEAPU32 && STATE.instance.HEAPU32.buffer);
                if (!mem) return [];
                const u32 = new Uint32Array(mem, hitPtr, count);
                return Array.from(u32);
            },
            renderOverlay(stageW, stageH, dx = 0, dy = 0, lineThick = 2.0) {
                if (!STATE.ready) return;
                const fn = getOptionalExport('wasm_render_selection_overlay');
                if (fn) {
                    fn(stageW, stageH, dx, dy, lineThick);
                }
            },
            clearOverlay(stageW, stageH) {
                if (!STATE.ready) return;
                const fn = getOptionalExport('wasm_clear_selection_overlay');
                if (fn) {
                    fn(stageW, stageH);
                }
            }
        },
        get pos() {
            return this.position;
        },
        getPathParams(slot) {
            return this.position ? this.position.getPathParams(slot) : null;
        },
        getOptionalExport(name) {
            return getOptionalExport(name);
        },
        getExports() { return STATE.exports; },
        RENDER_MODE_CODES: RENDER_MODE_CODES,
        RENDER_MODE_NAMES: RENDER_MODE_NAMES
    };
    if (typeof window !== 'undefined') {
        window.WasmBridge = WasmBridge;
        // Táº£i vÃ  khá»Ÿi táº¡o Wasm ngay láº­p tá»©c song song vá»›i quÃ¡ trÃ¬nh táº£i trang
        WasmBridge.init();
    }
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = WasmBridge;
    }
})(typeof window !== 'undefined' ? window : this);
