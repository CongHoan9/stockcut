
    function preloadProjectFonts(items, templates, refreshPreview = true) {
        if (!window.NativeFontAssets || typeof NativeFontAssets.ensureNativeBinary !== 'function') return Promise.resolve([]);
        const fontSet = new Set();
        const visited = new Set();
        let hasTextOrLyric = false;
        const scan = (obj) => {
            if (!obj || typeof obj !== 'object' || visited.has(obj)) return;
            visited.add(obj);
            const type = obj.type;
            if (type === 'lyric' || type === 'text' || type === 'caption') {
                hasTextOrLyric = true;
            }
            const fam = obj['font-family'] || obj.fontFamily || (obj.params && (obj.params['font-family'] || obj.params.fontFamily));
            if (fam && typeof fam === 'string' && fam.trim()) fontSet.add(fam.trim());
            for (const value of Object.values(obj)) scan(value);
        };
        if (Array.isArray(templates)) templates.forEach(scan);
        if (Array.isArray(items)) items.forEach(scan);
        if (hasTextOrLyric && fontSet.size === 0) {
            const defFam = (window.NativeFontAssets && typeof NativeFontAssets.getDefaultFamily === 'function')
                ? NativeFontAssets.getDefaultFamily()
                : '';
            if (defFam) fontSet.add(defFam);
        }
        return Promise.all([...fontSet].map(fam => NativeFontAssets.ensureNativeBinary(fam).catch(() => false)))
            .then(results => {
                if (window.WasmBridge && window.WasmBridge.renderer && typeof window.WasmBridge.renderer.invalidateTextCaches === 'function') {
                    window.WasmBridge.renderer.invalidateTextCaches();
                }
                if (refreshPreview && fontSet.size > 0 && typeof updateFrame === 'function') {
                    updateFrame(getAccurateTimeMs());
                }
                return results;
            });
    }
// ============================================================================
// [TIMELINE ENGINE v6] — ULTRA-THIN BROWSER RESOURCE & I/O ADAPTER
//
// Tuân thủ nghiêm ngặt GEMINI.md Rule 9 (JS Zero-Business Policy):
// 1. Toàn bộ logic nghiệp vụ (Timeline evaluation, Keyframe interpolation,
//    Effect math, Text shaping & reveal, Bass audio temporal bitboards,
//    VBO quad packing, Shader passes, 2-tier Caching) nằm 100% trong C++ Core.
// 2. JavaScript đóng vai trò là một lớp màng siêu mỏng (Thin Adapter):
//    - I/O: Nạp chuỗi JSON dự án -> WasmBridge.manager.loadProjectJson.
//    - Asset: Nạp file âm thanh / hình ảnh -> Blob URL / WebGL texture pointer.
//    - Clock Hook: Lắng nghe nhịp render -> 1 lệnh C++ master duy nhất.
//    - Hardware Sync: Đồng bộ currentTime / volume của <video> / <audio>.
// ============================================================================
(function () {
    'use strict';
    const global = typeof window !== 'undefined' ? window : globalThis;
    const state = global.state || (global.state = {});
    // State & Configurations
    state.timeline = state.timeline || [];
    state.config = state.config || {};
    state.renderConfig = state.renderConfig || {};
    state.timelineMeta = state.timelineMeta || { stage: {}, stylesheet: '', animations: {} };
    const OPT = {
        nativeCompositor: true,
        dirtyRect: true,
        skipStaticFrames: true
    };
    const dirtyStats = {
        full: 0, dirty: 0, none: 0, rects: 0, px: 0,
        zobristBreaker: 0, ttHit: 0, ttMiss: 0,
        runBuild: 0, runBlit: 0, sprBuild: 0, sprDraw: 0,
        frozen: 0, culled: 0
    };
    // ------------------------------------------------------------------------
    // 64-BIT PROPERTY BITMASK ARCHITECTURE (STOCKFISH-STYLE BITBOARD PARITY)
    // ------------------------------------------------------------------------
    const PropertyBit = Object.freeze({
        // Spatial & Geometry (Bits 0-7)
        PROP_LEFT:              1n << 0n,
        PROP_TOP:               1n << 1n,
        PROP_WIDTH:             1n << 2n,
        PROP_HEIGHT:            1n << 3n,
        PROP_ROTATE:            1n << 4n,
        PROP_SCALE:             1n << 5n,
        PROP_TRANSFORM:         1n << 6n,
        PROP_OPACITY:           1n << 7n,
        // Content & Typography (Bits 8-15)
        PROP_CONTENT:           1n << 8n,
        PROP_FONT_SIZE:         1n << 9n,
        PROP_FONT_FAMILY:       1n << 10n,
        PROP_FONT_WEIGHT:       1n << 11n,
        PROP_LINE_HEIGHT:       1n << 12n,
        PROP_LETTER_SPACING:    1n << 13n,
        PROP_TEXT_ALIGN:        1n << 14n,
        PROP_COLOR:             1n << 15n,
        // Styles & Effects (Bits 16-23)
        PROP_STROKE_WIDTH:      1n << 16n,
        PROP_STROKE_COLOR:      1n << 17n,
        PROP_BACKGROUND_COLOR:  1n << 18n,
        PROP_BACKGROUND:        1n << 19n,
        PROP_REVEAL_MODE:       1n << 20n,
        PROP_REVEAL_DURATION:   1n << 21n,
        PROP_REVEAL_STAGGER:    1n << 22n,
        PROP_MASK_HOLE:         1n << 23n,
        // Media & Dynamics (Bits 24-31)
        PROP_SPIN_SPEED:        1n << 24n,
        PROP_VOLUME:            1n << 25n,
        PROP_Z_INDEX:           1n << 26n,
        PROP_FILTER:            1n << 27n,
        PROP_BASS_ACTIVE:       1n << 28n,
        PROP_BASS_CUTOFF:       1n << 29n,
        PROP_BASS_SENSITIVITY:  1n << 30n,
        PROP_BASS_THRESHOLD:    1n << 31n,
        // Timing & Structure (Bits 32-39)
        PROP_START:             1n << 32n,
        PROP_END:               1n << 33n,
        PROP_DURATION:          1n << 34n,
        PROP_CLASS_NAME:        1n << 35n,
        PROP_SRC:               1n << 36n,
        PROP_TYPE:              1n << 37n,
        PROP_EFFECT:            1n << 38n,
        PROP_MUTED:             1n << 39n,
        // Reserved & Future Extensions (Bits 40-63)
        PROP_SHADOW_COLOR:      1n << 40n,
        PROP_SHADOW_BLUR:       1n << 41n,
        PROP_SHADOW_OFFSET_X:   1n << 42n,
        PROP_SHADOW_OFFSET_Y:   1n << 43n,
        PROP_BORDER_RADIUS:     1n << 44n,
        PROP_BORDER_COLOR:      1n << 45n,
        PROP_BORDER_WIDTH:      1n << 46n,
        PROP_PADDING:           1n << 47n,
        PROP_MARGIN:            1n << 48n,
        PROP_DISPLAY:           1n << 49n,
        PROP_VISIBILITY:        1n << 50n,
        PROP_OVERFLOW:          1n << 51n,
        PROP_ORIGIN_X:          1n << 52n,
        PROP_ORIGIN_Y:          1n << 53n,
        PROP_SKEW_X:            1n << 54n,
        PROP_SKEW_Y:            1n << 55n,
        PROP_PERSPECTIVE:       1n << 56n,
        PROP_BLEND_MODE:        1n << 57n,
        PROP_AUDIO_PLAYBACK_RATE: 1n << 58n,
        PROP_AUDIO_PRE_ROLL:    1n << 59n,
        PROP_VIDEO_LOOP:        1n << 60n,
        PROP_VIDEO_AUTOPLAY:    1n << 61n,
        PROP_CUSTOM_1:          1n << 62n,
        PROP_CUSTOM_2:          1n << 63n,
        // Compound Bitmasks
        MASK_SPATIAL_GEOM:      0x000000000000000Fn,
        MASK_SPATIAL_AFFINE:    0x0000000000000070n,
        MASK_SPATIAL_ALL:       0x000000000000007Fn,
        MASK_VISUAL_STATE:      0x0000000000FFFFFFn,
        MASK_COLOR_ALL:         (1n << 15n) | (1n << 17n) | (1n << 18n),
        MASK_TEXT_STYLING:      0x000000000000FF00n
    });
    function countBits64(mask) {
        let count = 0;
        let v = BigInt(mask || 0n);
        while (v > 0n) {
            v &= (v - 1n);
            count++;
        }
        return count;
    }
    function isPureStatic(keyframedMask) {
        return BigInt(keyframedMask || 0n) === 0n;
    }
    function isOpacityOnlyVariance(keyframedMask) {
        return BigInt(keyframedMask || 0n) === PropertyBit.PROP_OPACITY;
    }
    function hasSpatialVariance(keyframedMask) {
        return (BigInt(keyframedMask || 0n) & PropertyBit.MASK_SPATIAL_ALL) !== 0n;
    }
    const PROP_KEY_MAP = {
        'left': PropertyBit.PROP_LEFT, 'x': PropertyBit.PROP_LEFT,
        'top': PropertyBit.PROP_TOP, 'y': PropertyBit.PROP_TOP,
        'width': PropertyBit.PROP_WIDTH, 'w': PropertyBit.PROP_WIDTH,
        'height': PropertyBit.PROP_HEIGHT, 'h': PropertyBit.PROP_HEIGHT,
        'rotate': PropertyBit.PROP_ROTATE, 'rotation': PropertyBit.PROP_ROTATE,
        'scale': PropertyBit.PROP_SCALE, 'zoom': PropertyBit.PROP_SCALE,
        'transform': PropertyBit.PROP_TRANSFORM,
        'opacity': PropertyBit.PROP_OPACITY, 'alpha': PropertyBit.PROP_OPACITY,
        'content': PropertyBit.PROP_CONTENT, 'text': PropertyBit.PROP_CONTENT,
        'rawText': PropertyBit.PROP_CONTENT, 'lyric': PropertyBit.PROP_CONTENT, 'lyrics': PropertyBit.PROP_CONTENT,
        'fontSize': PropertyBit.PROP_FONT_SIZE, 'font-size': PropertyBit.PROP_FONT_SIZE,
        'fontFamily': PropertyBit.PROP_FONT_FAMILY, 'font-family': PropertyBit.PROP_FONT_FAMILY,
        'fontWeight': PropertyBit.PROP_FONT_WEIGHT, 'font-weight': PropertyBit.PROP_FONT_WEIGHT,
        'lineHeight': PropertyBit.PROP_LINE_HEIGHT, 'line-height': PropertyBit.PROP_LINE_HEIGHT,
        'letterSpacing': PropertyBit.PROP_LETTER_SPACING, 'letter-spacing': PropertyBit.PROP_LETTER_SPACING,
        'textAlign': PropertyBit.PROP_TEXT_ALIGN, 'text-align': PropertyBit.PROP_TEXT_ALIGN,
        'color': PropertyBit.PROP_COLOR, 'fill': PropertyBit.PROP_COLOR,
        'strokeWidth': PropertyBit.PROP_STROKE_WIDTH, 'stroke-width': PropertyBit.PROP_STROKE_WIDTH,
        'strokeColor': PropertyBit.PROP_STROKE_COLOR, 'stroke-color': PropertyBit.PROP_STROKE_COLOR, 'stroke': PropertyBit.PROP_STROKE_COLOR,
        'backgroundColor': PropertyBit.PROP_BACKGROUND_COLOR, 'background-color': PropertyBit.PROP_BACKGROUND_COLOR,
        'background': PropertyBit.PROP_BACKGROUND, 'bg': PropertyBit.PROP_BACKGROUND,
        'revealMode': PropertyBit.PROP_REVEAL_MODE, 'reveal-mode': PropertyBit.PROP_REVEAL_MODE,
        'revealDuration': PropertyBit.PROP_REVEAL_DURATION, 'reveal-duration': PropertyBit.PROP_REVEAL_DURATION,
        'revealStagger': PropertyBit.PROP_REVEAL_STAGGER, 'reveal-stagger': PropertyBit.PROP_REVEAL_STAGGER,
        'maskHole': PropertyBit.PROP_MASK_HOLE, 'mask-hole': PropertyBit.PROP_MASK_HOLE,
        'spinSpeed': PropertyBit.PROP_SPIN_SPEED, 'spin-speed': PropertyBit.PROP_SPIN_SPEED, 'spin': PropertyBit.PROP_SPIN_SPEED,
        'volume': PropertyBit.PROP_VOLUME, 'volunm': PropertyBit.PROP_VOLUME, 'vol': PropertyBit.PROP_VOLUME, 'level': PropertyBit.PROP_VOLUME, 'gain': PropertyBit.PROP_VOLUME,
        'zIndex': PropertyBit.PROP_Z_INDEX, 'z-index': PropertyBit.PROP_Z_INDEX, 'z': PropertyBit.PROP_Z_INDEX,
        'filter': PropertyBit.PROP_FILTER,
        'bassActive': PropertyBit.PROP_BASS_ACTIVE, 'bass-active': PropertyBit.PROP_BASS_ACTIVE,
        'bassCutoff': PropertyBit.PROP_BASS_CUTOFF, 'bass-cutoff': PropertyBit.PROP_BASS_CUTOFF,
        'bassSensitivity': PropertyBit.PROP_BASS_SENSITIVITY, 'bass-sensitivity': PropertyBit.PROP_BASS_SENSITIVITY,
        'bassThreshold': PropertyBit.PROP_BASS_THRESHOLD, 'bass-threshold': PropertyBit.PROP_BASS_THRESHOLD,
        'start': PropertyBit.PROP_START,
        'end': PropertyBit.PROP_END,
        'duration': PropertyBit.PROP_DURATION, 'durationMs': PropertyBit.PROP_DURATION,
        'class': PropertyBit.PROP_CLASS_NAME, 'className': PropertyBit.PROP_CLASS_NAME,
        'src': PropertyBit.PROP_SRC, 'source': PropertyBit.PROP_SRC, 'url': PropertyBit.PROP_SRC,
        'type': PropertyBit.PROP_TYPE,
        'effect': PropertyBit.PROP_EFFECT,
        'muted': PropertyBit.PROP_MUTED
    };
    function computeItemPropertyMasks(it) {
        if (typeof PropertyModule !== 'undefined' && PropertyModule.computeItemPropertyMasks) {
            return PropertyModule.computeItemPropertyMasks(it);
        }
        if (!it || typeof it !== 'object') {
            return { definedProps: 0n, keyframedProps: 0n, isStatic: true, isOpacityOnly: false };
        }
        let defMask = 0n;
        let kfMask = 0n;
        // Inspect root fields
        for (const k in it) {
            const bit = PROP_KEY_MAP[k];
            if (bit !== undefined && it[k] !== undefined && it[k] !== null) {
                defMask |= bit;
            }
        }
        // Inspect css / style / params sub-objects
        const subObjs = [it.css, it.style, it.params];
        for (const sub of subObjs) {
            if (sub && typeof sub === 'object') {
                for (const k in sub) {
                    const cleanK = k.startsWith('--') ? k.slice(2) : k;
                    const bit = PROP_KEY_MAP[cleanK] || PROP_KEY_MAP[k];
                    if (bit !== undefined && sub[k] !== undefined && sub[k] !== null) {
                        defMask |= bit;
                    }
                }
            }
        }
        // Inspect keyframes
        const rawKfs = it.keyframes || it.keyframe || it.keyFrames ||
            (it.css && (it.css.keyframes || it.css.keyframe)) ||
            (it.style && (it.style.keyframes || it.style.keyframe));
        if (Array.isArray(rawKfs) && rawKfs.length > 0) {
            for (let i = 0; i < rawKfs.length; i++) {
                const kf = rawKfs[i];
                if (!kf || typeof kf !== 'object') continue;
                for (const prop in kf) {
                    if (prop === 'offset' || prop === 'offsetMs' || prop === 'time' || prop === 't' || prop === 'at') continue;
                    const bit = PROP_KEY_MAP[prop];
                    if (bit !== undefined && kf[prop] !== undefined) {
                        kfMask |= bit;
                        defMask |= bit;
                    }
                }
                const kfSub = [kf.css, kf.style];
                for (const sub of kfSub) {
                    if (sub && typeof sub === 'object') {
                        for (const prop in sub) {
                            const cleanP = prop.startsWith('--') ? prop.slice(2) : prop;
                            const bit = PROP_KEY_MAP[cleanP] || PROP_KEY_MAP[prop];
                            if (bit !== undefined && sub[prop] !== undefined) {
                                kfMask |= bit;
                                defMask |= bit;
                            }
                        }
                    }
                }
            }
        }
        const isStatic = (kfMask === 0n);
        const isOpacityOnly = (kfMask === PropertyBit.PROP_OPACITY);
        return { definedProps: defMask, keyframedProps: kfMask, isStatic, isOpacityOnly };
    }
    function attachPropertyMasks(items) {
        if (typeof PropertyModule !== 'undefined' && PropertyModule.attachPropertyMasks) {
            return PropertyModule.attachPropertyMasks(items);
        }
        if (!Array.isArray(items)) return;
        for (let i = 0; i < items.length; i++) {
            const it = items[i];
            if (!it) continue;
            const masks = computeItemPropertyMasks(it);
            it._definedProps = masks.definedProps;
            it._keyframedProps = masks.keyframedProps;
            it._isStatic = masks.isStatic;
            it._isOpacityOnly = masks.isOpacityOnly;
        }
    }
    // ------------------------------------------------------------------------
    // 1. ASSET I/O & BLOB MANAGEMENT
    // ------------------------------------------------------------------------
    const assetUrls = new Map();
    let assetDir = null;
    function registerAssetBlob(path, blobUrl, extra) {
        if (!path || !blobUrl) return;
        if (window.ResourceSystem && typeof window.ResourceSystem.registerAsset === 'function') {
            const desc = window.ResourceSystem.registerAsset(path, blobUrl, extra || {});
            if (desc && desc.url) blobUrl = desc.url;
        }
        assetUrls.set(path, blobUrl);
        const clean = path.replace(/^\/+/, '');
        assetUrls.set(clean, blobUrl);
        assetUrls.set('/' + clean, blobUrl);
        if (clean.toLowerCase().startsWith('project/')) {
            const sub = clean.slice(8);
            assetUrls.set(sub, blobUrl);
            assetUrls.set('/' + sub, blobUrl);
        }
        const baseName = clean.split('/').filter(Boolean).pop();
        if (baseName) {
            assetUrls.set(baseName, blobUrl);
            assetUrls.set('/' + baseName, blobUrl);
        }
    }
    function unregisterAssetBlob(path) {
        if (!path) return;
        assetUrls.delete(path);
        assetUrls.delete(path.replace(/^\/+/, ''));
    }
    function getAssetBlob(src) {
        if (!src) return null;
        if (assetUrls.has(src)) return assetUrls.get(src);
        if (window.ResourceSystem && typeof window.ResourceSystem.resolveDescriptor === 'function') {
            const desc = window.ResourceSystem.resolveDescriptor(src);
            if (desc && desc.url) return desc.url;
        }
        return null;
    }
    function resolveSrc(src) {
        if (!src) return '';
        if (assetUrls.has(src)) return assetUrls.get(src);
        const clean = src.replace(/^\/+/, '');
        if (assetUrls.has(clean)) return assetUrls.get(clean);
        if (assetUrls.has('/' + clean)) return assetUrls.get('/' + clean);
        if (clean.toLowerCase().startsWith('project/')) {
            const sub = clean.slice(8);
            if (assetUrls.has(sub)) return assetUrls.get(sub);
            if (assetUrls.has('/' + sub)) return assetUrls.get('/' + sub);
        }
        if (window.ResourceSystem && typeof window.ResourceSystem.resolveUrl === 'function') {
            const resolved = window.ResourceSystem.resolveUrl(src);
            if (resolved) return resolved;
        }
        if (typeof window.resolveFileUrl === 'function') {
            return window.resolveFileUrl(src);
        }
        return src;
    }
    async function getFileHandleByPath(dirHandle, path) {
        if (!dirHandle || !path) throw new Error('Invalid dirHandle or path');
        const cleanPath = String(path).trim().replace(/^\/+/, '').replace(/^\.\//, '');
        try {
            const parts = cleanPath.split('/').filter(p => p && p !== '.');
            let dir = dirHandle;
            for (let i = 0; i < parts.length - 1; i++) {
                dir = await dir.getDirectoryHandle(parts[i]);
            }
            const fileHandle = await dir.getFileHandle(parts[parts.length - 1]);
            const file = await fileHandle.getFile();
            return { fileHandle, file };
        } catch (e1) {
            if (cleanPath.toLowerCase().startsWith('project/')) {
                const subPath = cleanPath.slice(8);
                const parts = subPath.split('/').filter(p => p && p !== '.');
                let dir = dirHandle;
                for (let i = 0; i < parts.length - 1; i++) {
                    dir = await dir.getDirectoryHandle(parts[i]);
                }
                const fileHandle = await dir.getFileHandle(parts[parts.length - 1]);
                const file = await fileHandle.getFile();
                return { fileHandle, file };
            }
            try {
                const filename = cleanPath.split('/').filter(Boolean).pop();
                if (filename) {
                    const fileHandle = await dirHandle.getFileHandle(filename);
                    const file = await fileHandle.getFile();
                    return { fileHandle, file };
                }
            } catch (_) {}
            throw e1;
        }
    }
    async function getFileByPath(dirHandle, path) {
        const res = await getFileHandleByPath(dirHandle, path);
        return res.file;
    }
    async function preloadAssets(dirHandle) {
        const epoch = window.ResourceSystem ? window.ResourceSystem.currentProjectEpoch() : 0;
        if (dirHandle && assetDir !== dirHandle) {
            for (const url of assetUrls.values()) {
                if (url && url.startsWith('blob:')) {
                    try { URL.revokeObjectURL(url); } catch (e) { }
                }
            }
            assetUrls.clear();
            assetDir = dirHandle;
        }
        const missing = [];
        const srcs = new Set();
        function collectSrcs(items) {
            if (!Array.isArray(items)) return;
            for (const it of items) {
                if (!it) continue;
                if (it.src && !it.src.startsWith('data:') && !it.src.startsWith('blob:') && !it.src.startsWith('http')) {
                    srcs.add(it.src);
                }
                if (Array.isArray(it.childrent)) collectSrcs(it.childrent);
                if (Array.isArray(it.children)) collectSrcs(it.children);
            }
        }
        collectSrcs(state.timeline);
        if (!dirHandle) {
            await Promise.all(Array.from(srcs).map(async (src) => {
                if (/\.(png|jpe?g|webp|gif|svg)$/i.test(src) && window.ResourceSystem) {
                    await window.ResourceSystem.loadDecodedImage(src, () => {
                        if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                            global.CanvasCompositor.invalidateScene('ASSET_LOADED');
                        }
                    });
                }
            }));
            if (epoch && window.ResourceSystem && window.ResourceSystem.currentProjectEpoch() !== epoch) {
                return missing;
            }
            if (state.timeline && state.timeline.some(it => (it.params && it.params.source === 'bass') || it.source === 'bass')) {
                prepareBass();
            }
            return missing;
        }
        let newBlobsRegistered = false;
        await Promise.all(Array.from(srcs).map(async (src) => {
            if (assetUrls.has(src)) {
                if (/\.(png|jpe?g|webp|gif|svg)$/i.test(src) && window.ResourceSystem) {
                    await window.ResourceSystem.loadDecodedImage(src);
                }
                return;
            }
            try {
                const { fileHandle, file } = await getFileHandleByPath(dirHandle, src);
                let diskAbsPath = '';
                let diskStreamUrl = '';
                if (window.StockCutDiskBridge) {
                    const ctx = state.projectContext || {};
                    const extraDirs = ctx.rootPath ? [ctx.rootPath] : [];
                    diskAbsPath = await window.StockCutDiskBridge.resolveFilePath(file.name, file.size || 0, extraDirs);
                    if (diskAbsPath) {
                        diskStreamUrl = window.StockCutDiskBridge.toFsUrl(diskAbsPath);
                        window.StockCutDiskBridge.addLink({
                            absPath: diskAbsPath,
                            name: file.name,
                            size: file.size || 0
                        }).catch(() => {});
                    }
                }
                registerAssetBlob(src, diskStreamUrl || file, {
                    handle: fileHandle,
                    file: diskStreamUrl ? null : file,
                    blob: diskStreamUrl ? null : file,
                    absPath: diskAbsPath || '',
                    relPath: src,
                    name: file.name,
                    byteLength: file.size || 0,
                    lastModified: file.lastModified || 0
                });
                newBlobsRegistered = true;
                if (/\.(png|jpe?g|webp|gif|svg)$/i.test(src) || (file.type && file.type.startsWith('image/'))) {
                    if (window.ResourceSystem) {
                        await window.ResourceSystem.loadDecodedImage(src, () => {
                            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                                global.CanvasCompositor.invalidateScene('ASSET_LOADED');
                            }
                        });
                    }
                }
            } catch (e) {
                const cleanSrc = src.replace(/^\/+/, '');
                const fetchUrl = cleanSrc.startsWith('Asset/') ? cleanSrc : (src.startsWith('/') ? src : ('/' + cleanSrc));
                try {
                    const res = await fetch(fetchUrl, { method: 'HEAD' });
                    if (res.ok) {
                        registerAssetBlob(src, fetchUrl);
                        if (/\.(png|jpe?g|webp|gif|svg)$/i.test(src) && window.ResourceSystem) {
                            await window.ResourceSystem.loadDecodedImage(src, () => {
                                if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                                    global.CanvasCompositor.invalidateScene('ASSET_LOADED');
                                }
                            });
                        }
                        return;
                    }
                } catch (_) {}
                missing.push(src);
            }
        }));
        if (epoch && window.ResourceSystem && window.ResourceSystem.currentProjectEpoch() !== epoch) {
            return missing;
        }
        if (state.timeline && state.timeline.some(it => (it.params && it.params.source === 'bass') || it.source === 'bass')) prepareBass();
        return missing;
    }
    // ------------------------------------------------------------------------
    // 2. BROWSER MEDIA HARDWARE SYNC & AUDIO KEYFRAME ENVELOPE ENGINE
    // ------------------------------------------------------------------------
    const mediaPool = new Map();
    function getAudioItemBaseVolume(it) {
        if (!it) return 1.0;
        let v = it.volume !== undefined ? it.volume
            : (it.volunm !== undefined ? it.volunm
            : (it.vol !== undefined ? it.vol
            : (it.level !== undefined ? it.level
            : (it.gain !== undefined ? it.gain
            : (it.value !== undefined ? it.value
            : (it.val !== undefined ? it.val : undefined))))));
        if (v === undefined && it.css) {
            v = it.css['--volume'] !== undefined ? it.css['--volume']
                : (it.css.volume !== undefined ? it.css.volume
                : (it.css.volunm !== undefined ? it.css.volunm : undefined));
        }
        if (v === undefined && it.style) {
            v = it.style['--volume'] !== undefined ? it.style['--volume']
                : (it.style.volume !== undefined ? it.style.volume
                : (it.style.volunm !== undefined ? it.style.volunm : undefined));
        }
        if (typeof v === 'string') {
            const s = v.trim();
            if (s.endsWith('%')) {
                const n = parseFloat(s);
                return Number.isFinite(n) ? Math.max(0, Math.min(2, n / 100)) : 1.0;
            }
            const n = parseFloat(s);
            return Number.isFinite(n) ? Math.max(0, Math.min(2, n)) : 1.0;
        }
        if (Number.isFinite(v)) {
            return Math.max(0, Math.min(2, v));
        }
        return 1.0;
    }
    function parseAudioKeyframeVolume(kf) {
        if (!kf) return 1.0;
        let v = kf.volume !== undefined ? kf.volume
            : (kf.volunm !== undefined ? kf.volunm
            : (kf.vol !== undefined ? kf.vol
            : (kf.level !== undefined ? kf.level
            : (kf.gain !== undefined ? kf.gain
            : (kf.value !== undefined ? kf.value
            : (kf.val !== undefined ? kf.val
            : (kf.v !== undefined ? kf.v : undefined)))))));
        if (v === undefined && kf.css) {
            v = kf.css['--volume'] !== undefined ? kf.css['--volume']
                : (kf.css.volume !== undefined ? kf.css.volume
                : (kf.css.volunm !== undefined ? kf.css.volunm : undefined));
        }
        if (v === undefined && kf.style) {
            v = kf.style['--volume'] !== undefined ? kf.style['--volume']
                : (kf.style.volume !== undefined ? kf.style.volume
                : (kf.style.volunm !== undefined ? kf.style.volunm : undefined));
        }
        if (typeof v === 'string') {
            const s = v.trim();
            if (s.endsWith('%')) {
                const n = parseFloat(s);
                return Number.isFinite(n) ? Math.max(0, Math.min(2, n / 100)) : 1.0;
            }
            const n = parseFloat(s);
            return Number.isFinite(n) ? Math.max(0, Math.min(2, n)) : 1.0;
        }
        if (Number.isFinite(v)) {
            return Math.max(0, Math.min(2, v));
        }
        return 1.0;
    }
    function parseAudioKeyframeOffset(kf, clipDurationMs) {
        if (!kf) return 0;
        const dur = (Number.isFinite(clipDurationMs) && clipDurationMs > 0) ? clipDurationMs : 1000;
        let off = kf.offset !== undefined ? kf.offset
            : (kf.offsetMs !== undefined ? kf.offsetMs
            : (kf.time !== undefined ? kf.time
            : (kf.t !== undefined ? kf.t
            : (kf.at !== undefined ? kf.at : 0))));
        if (typeof off === 'string') {
            const s = off.trim();
            if (s.endsWith('%')) {
                const p = parseFloat(s);
                return Number.isFinite(p) ? (p / 100 * dur) : 0;
            }
            if (s.endsWith('ms')) {
                const n = parseFloat(s);
                return Number.isFinite(n) ? n : 0;
            }
            if (s.endsWith('s')) {
                const n = parseFloat(s);
                return Number.isFinite(n) ? (n * 1000) : 0;
            }
            const n = parseFloat(s);
            if (Number.isFinite(n)) {
                if (n >= 0 && n <= 1.0 && dur > 1000) return n * dur;
                return n;
            }
            return 0;
        }
        if (Number.isFinite(off)) {
            if (off > 0 && off <= 1.0 && dur > 1000) return off * dur;
            return off;
        }
        return 0;
    }
    function getAudioEnvelopePoints(it) {
        const baseVol = getAudioItemBaseVolume(it);
        const rawKfs = (it && (it.keyframes || it.keyframe || it.keyFrames || (it.css && (it.css.keyframes || it.css.keyframe)) || (it.style && (it.style.keyframes || it.style.keyframe)))) || null;
        if (!Array.isArray(rawKfs) || !rawKfs.length) {
            return {
                baseVol,
                initialVolume: baseVol,
                points: []
            };
        }
        const clipDur = (it.end !== undefined && it.start !== undefined) ? Math.max(0, it.end - it.start) : 0;
        const sorted = rawKfs.map(k => ({
            offsetMs: parseAudioKeyframeOffset(k, clipDur),
            rawVol: parseAudioKeyframeVolume(k)
        })).sort((a, b) => a.offsetMs - b.offsetMs);
        let initialVolume = baseVol;
        if (sorted.length > 0 && sorted[0].offsetMs === 0) {
            initialVolume = sorted[0].rawVol * baseVol;
        }
        const points = sorted.map(pt => ({
            offsetMs: pt.offsetMs,
            volume: pt.rawVol * baseVol
        }));
        return {
            baseVol,
            initialVolume,
            points
        };
    }
    function evaluateAudioItemVolume(it, nowMs) {
        if (!it) return 1.0;
        const env = getAudioEnvelopePoints(it);
        if (!env.points.length) {
            return Math.max(0, Math.min(1, env.baseVol));
        }
        const start = Number(it.start) || 0;
        const relMs = nowMs - start;
        const pts = env.points;
        if (pts.length === 1 || relMs <= pts[0].offsetMs) {
            return Math.max(0, Math.min(1, pts[0].volume));
        }
        const last = pts.length - 1;
        if (relMs >= pts[last].offsetMs) {
            return Math.max(0, Math.min(1, pts[last].volume));
        }
        for (let i = 0; i < last; i++) {
            const t0 = pts[i].offsetMs;
            const t1 = pts[i + 1].offsetMs;
            if (relMs >= t0 && relMs <= t1) {
                const span = t1 - t0;
                const alpha = span > 0 ? (relMs - t0) / span : 1.0;
                const interpolated = pts[i].volume + (pts[i + 1].volume - pts[i].volume) * alpha;
                return Math.max(0, Math.min(1, interpolated));
            }
        }
        return Math.max(0, Math.min(1, env.baseVol));
    }
    function safePlay(media) {
        if (!media || !media.paused) return;
        if (media._playPending) return;
        media._playPending = true;
        const p = media.play();
        if (p && typeof p.then === 'function') {
            p.then(() => {
                media._playPending = false;
                if (media._pausePending) {
                    media._pausePending = false;
                    try { media.pause(); } catch (e) {}
                }
            }).catch(() => {
                media._playPending = false;
            });
        } else {
            media._playPending = false;
        }
    }
    function safePause(media) {
        if (!media || media.paused) return;
        if (media._playPending) {
            media._pausePending = true;
            return;
        }
        try { media.pause(); } catch (e) {}
    }
    function applyNativeAudioSync(media, slot, nowMs, isPlaying, item) {
        if (!media) return;
        const targetVol = item ? evaluateAudioItemVolume(item, nowMs) : null;
        const bridge = window.WasmBridge;
        if (bridge && bridge.audioEngine && bridge.isReady()) {
            const wallNowMs = performance.now();
            const res = bridge.audioEngine.syncTrack(
                slot, nowMs, media.currentTime, wallNowMs, isPlaying, media.paused, media.seeking
            );
            if (res) {
                const mask = res.actionMask;
                if ((mask & 8) && !media.paused) {
                    safePause(media);
                }
                if ((mask & 4) && media.paused) {
                    safePlay(media);
                }
                if (mask & 1) { // Chỉ seek khi C++ ra lệnh: drift > 450ms và hết cooldown 400ms!
                    if (media.readyState >= 1) {
                        try { media.currentTime = res.targetSec; } catch (e) { }
                    }
                }
                const finalVol = (targetVol !== null && Number.isFinite(targetVol))
                    ? targetVol
                    : Math.max(0, Math.min(1, res.targetVolume));
                const safeVol = Math.max(0, Math.min(1, finalVol));
                if (Math.abs(media.volume - safeVol) > 0.002) {
                    media.volume = safeVol;
                }
                return;
            }
        }
        // Fallback an toàn (chống rè: không seek liên tục trong lúc đang phát)
        const startMs = (item && Number.isFinite(item.start)) ? item.start : 0;
        const targetSec = Math.max(0, nowMs - startMs) / 1000;
        if (isPlaying) {
            safePlay(media);
            if (media.readyState >= 1 && Math.abs(media.currentTime - targetSec) > 0.45) {
                try { media.currentTime = targetSec; } catch (e) { }
            }
        } else {
            safePause(media);
            if (media.readyState >= 1 && Math.abs(media.currentTime - targetSec) > 0.05) {
                try { media.currentTime = targetSec; } catch (e) { }
            }
        }
        if (targetVol !== null && Number.isFinite(targetVol)) {
            const safeVol = Math.max(0, Math.min(1, targetVol));
            if (Math.abs(media.volume - safeVol) > 0.002) {
                media.volume = safeVol;
            }
        }
    }
    let mediaTracks = [];
    function bindCompiledTimeline(items) {
        state.timeline = items;
        mediaTracks = [];
        items.forEach((it, slot) => {
            if (it && (it.type === 'audio' || it.type === 'video')) {
                mediaTracks.push({ it, uid: it.uid || it.id || ('slot_' + slot), slot: it._nativeSlot ?? slot });
            }
        });
    }
    function syncMedia(nowMs, isPlaying) {
        // 0. Điều phối đường ống Streaming Random-Access 1:1 (C++ DemuxPipeline)
        if (window.WasmBridge && window.WasmBridge.stream) {
            window.WasmBridge.stream.scheduleAt(nowMs, isPlaying);
        }
        // 1. Đồng bộ mediaPlayer toàn cục (nếu có file nạp ngoài timeline) qua C++ slot 63
        if (typeof UI !== 'undefined' && UI.mediaPlayer && UI.mediaPlayer.src) {
            applyNativeAudioSync(UI.mediaPlayer, 63, nowMs, isPlaying, null);
        }
        // 2. Đồng bộ các audio/video items trong timeline qua C++ slot riêng
        for (const {it, uid, slot} of mediaTracks) {
            let rec = mediaPool.get(uid);
            const isActive = (nowMs >= it.start && nowMs <= it.end);
            if (isActive) {
                if (!rec) {
                    const el = document.createElement(it.type);
                    el.preload = 'auto';
                    if (it.type === 'video') {
                        el.muted = !!it.muted;
                        el.playsInline = true;
                    } else if (it.type === 'audio' && it.muted !== undefined) {
                        el.muted = !!it.muted;
                    }
                    const resolved = resolveSrc(it.src);
                    if (resolved) el.src = resolved;
                    const initialVol = evaluateAudioItemVolume(it, nowMs);
                    el.volume = initialVol;
                    rec = {
                        item: it,
                        rawSrc: it.src,
                        el,
                        video: it.type === 'video' ? el : null,
                        audio: it.type === 'audio' ? el : null
                    };
                    mediaPool.set(uid, rec);
                } else {
                    rec.item = it;
                    if (rec.rawSrc !== it.src) {
                        rec.rawSrc = it.src;
                        const resolved = resolveSrc(it.src);
                        if (resolved) rec.el.src = resolved;
                    }
                }
                applyNativeAudioSync(rec.el, slot, nowMs, isPlaying, it);
            } else if (rec && rec.el) {
                safePause(rec.el);
            }
        }
    }
    function unlockAudio() {
        for (const rec of mediaPool.values()) {
            if (rec && rec.el && rec.el.paused && state.isPlaying) {
                safePlay(rec.el);
            }
        }
    }
    function pauseAllMedia() {
        if (typeof UI !== 'undefined' && UI.mediaPlayer && !UI.mediaPlayer.paused) {
            try { UI.mediaPlayer.pause(); } catch (e) { }
        }
        for (const rec of mediaPool.values()) {
            if (rec && rec.el && !rec.el.paused) {
                safePause(rec.el);
            }
        }
    }
    function setAllPlaybackRate(rate) {
        const r = Number.isFinite(rate) && rate > 0 ? rate : 1;
        if (typeof UI !== 'undefined' && UI.mediaPlayer) {
            UI.mediaPlayer.playbackRate = r;
        }
        for (const rec of mediaPool.values()) {
            if (rec && rec.el && rec.el.playbackRate !== r) {
                rec.el.playbackRate = r;
            }
        }
    }
    function getPlaybackMediaTimeMs() {
        if (typeof UI !== 'undefined' && UI.mediaPlayer && UI.mediaPlayer.src &&
            !UI.mediaPlayer.paused && !UI.mediaPlayer.seeking) {
            return UI.mediaPlayer.currentTime * 1000;
        }
        for (const rec of mediaPool.values()) {
            const it = rec.item;
            if (it && it.type === 'audio' && rec.audio && !rec.audio.paused && !rec.audio.seeking) {
                return it.start + rec.audio.currentTime * 1000;
            }
        }
        return null;
    }
    // Lightweight stage wrapper for CanvasCompositor video texture lookups
    const stage = {
        items: {
            get(uid) { return mediaPool.get(uid) || null; }
        },
        cache: { w: 1920, h: 1080, bg: '#000000' },
        tile: { isNativeGL: true },
        isNativeGL: true,
        dirty: false
    };
    // ------------------------------------------------------------------------
    // 3. AUDIO DECODING & DSP SCRIPT BAKING
    // ------------------------------------------------------------------------
    let decodeCtx = null;
    function getDecodeCtx() {
        if (!decodeCtx || decodeCtx.state === 'closed') {
            const AC = window.AudioContext || window.webkitAudioContext;
            if (AC) try { decodeCtx = new AC(); } catch(e){}
            if (!decodeCtx) { const OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext; if (OAC) try { decodeCtx = new OAC(2, 44100, 44100); } catch(_) {} }
        }
        return decodeCtx;
    }
    function audioSources() {
        return (state.timeline || []).filter(it => (it.type === 'audio' || (it.type === 'video' && !it.muted)) && it.src);
    }
    function applyVolumeEnvelope(param, it, at) {
        if (!param || !it) return;
        const atTime = Number.isFinite(at) ? at : 0;
        const env = getAudioEnvelopePoints(it);
        try {
            param.cancelScheduledValues(atTime);
            param.setValueAtTime(env.initialVolume, atTime);
            for (const pt of env.points) {
                const targetTime = atTime + (pt.offsetMs / 1000);
                if (targetTime > atTime) {
                    param.linearRampToValueAtTime(pt.volume, targetTime);
                } else {
                    param.setValueAtTime(pt.volume, atTime);
                }
            }
        } catch (e) {
            try {
                param.value = env.initialVolume;
            } catch (_) {}
        }
    }
        // ------------------------------------------------------------------------
    // 3. AUDIO PRE-BAKING VIA C++ NATIVE ENGINE (GEMINI.md Rule 9)
    // ------------------------------------------------------------------------
    async function prepareBass() {
        const items = audioSources();
        if (!items.length) return;
        const OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
        if (!OAC) return;
        const dec = getDecodeCtx() || new OAC(1, 1, 44100);
        const plans = [];
        for (const it of items) {
            try {
                const buf = (window.ResourceSystem && typeof window.ResourceSystem.loadDecodedAudio === 'function')
                    ? await window.ResourceSystem.loadDecodedAudio(it.src, dec)
                    : await dec.decodeAudioData(await (await fetch(resolveSrc(it.src))).arrayBuffer());
                if (buf) plans.push({ it, buf });
            } catch (err) {
                console.warn('[TimelineEngine] Audio decoding error for', it.src, err);
            }
        }
        if (!plans.length) return;
        let cutoff = 150, sensitivity = 1.0, threshold = 0.2, release = 180;
        if (Array.isArray(state.timeline)) {
            for (const it of state.timeline) {
                if (it && (it.source === 'bass' || (it.params && it.params.source === 'bass'))) {
                    const p = it.params || it;
                    if (p.cutoff !== undefined) cutoff = Number(p.cutoff);
                    if (p.sensitivity !== undefined) sensitivity = Number(p.sensitivity);
                    if (p.threshold !== undefined) threshold = Number(p.threshold);
                    if (p.release !== undefined) release = Number(p.release);
                    break;
                }
            }
        }
        try {
            const durMs = getDurationMs() || (plans[0].buf.duration * 1000);
            const nativeSr = plans[0].buf.sampleRate || 44100;
            // Mix down audio tracks to mono PCM using WebAudio OfflineAudioContext
            const total = Math.max(2048 * 2, Math.ceil(durMs / 1000 * nativeSr));
            const off = new OAC(1, total, nativeSr);
            for (const { it, buf } of plans) {
                const durS = (it.end - it.start) / 1000;
                const at = it.start / 1000;
                const g = off.createGain();
                applyVolumeEnvelope(g.gain, it, at);
                const s = off.createBufferSource();
                s.buffer = buf;
                s.connect(g);
                g.connect(off.destination);
                s.start(at, 0, Math.min(durS, buf.duration));
            }
            const rendered = await off.startRendering();
            const samples = rendered.getChannelData(0);
            // 100% C++ Native STFT, SuperFlux, Moving Average Peak-Picking & Bitboard Baking
            if (window.WasmBridge && window.WasmBridge.manager && window.WasmBridge.isReady()) {
                window.WasmBridge.manager.analyzeAndBakeAudio(
                    samples, nativeSr, cutoff, sensitivity, threshold, release, durMs
                );
                state.bassEnvelope = window.WasmBridge.manager.getMusicScriptEnvelope();
                if (window.WasmBridge.ttClear) window.WasmBridge.ttClear();
            }
            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                global.CanvasCompositor.invalidateScene('BASS_READY');
            }
            window.dispatchEvent(new CustomEvent('timeline-bass-ready'));
            if (typeof global.updateFrame === 'function' && !state.isPlaying) {
                global.updateFrame();
            }
        } catch (e) {
            console.warn('[TimelineEngine] Native audio analysis error:', e);
        }
    }
    async function buildWaveformBuffer(durationMs) {
        const items = audioSources();
        if (!items.length || !(durationMs > 0)) return null;
        const sr = 8000;
        const total = Math.max(1, Math.ceil(durationMs / 1000 * sr));
        const OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
        const off = new OAC(1, total, sr);
        const dec = getDecodeCtx();
        if (!dec) return null;
        let any = false;
        for (const it of items) {
            try {
                const buf = (window.ResourceSystem && typeof window.ResourceSystem.loadDecodedAudio === 'function')
                    ? await window.ResourceSystem.loadDecodedAudio(it.src, dec)
                    : await dec.decodeAudioData(await (await fetch(resolveSrc(it.src))).arrayBuffer());
                const durS = (it.end - it.start) / 1000;
                const at = it.start / 1000;
                const g = off.createGain();
                applyVolumeEnvelope(g.gain, it, at);
                const src = off.createBufferSource();
                src.buffer = buf;
                src.connect(g); g.connect(off.destination);
                src.start(at, 0, Math.min(durS, buf.duration));
                any = true;
            } catch (e) {
                console.warn(`[Timeline] Waveform decode error for "${it.id}" (${it.src}):`, e);
            }
        }
        return any ? off.startRendering() : null;
    }
    async function buildHighResAudioBuffer(durationMs) {
        const items = audioSources();
        if (!items.length || !(durationMs > 0)) return null;
        const sr = 44100;
        const total = Math.max(1, Math.ceil(durationMs / 1000 * sr));
        const OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
        const off = new OAC(2, total, sr);
        const dec = getDecodeCtx();
        if (!dec) return null;
        let any = false;
        for (const it of items) {
            try {
                const buf = (window.ResourceSystem && typeof window.ResourceSystem.loadDecodedAudio === 'function')
                    ? await window.ResourceSystem.loadDecodedAudio(it.src, dec)
                    : await dec.decodeAudioData(await (await fetch(resolveSrc(it.src))).arrayBuffer());
                const durS = (it.end - it.start) / 1000;
                const at = it.start / 1000;
                const g = off.createGain();
                applyVolumeEnvelope(g.gain, it, at);
                const src = off.createBufferSource();
                src.buffer = buf;
                src.connect(g); g.connect(off.destination);
                src.start(at, 0, Math.min(durS, buf.duration));
                any = true;
            } catch (e) {
                console.warn(`[Timeline] High-res audio decode error for "${it.id}" (${it.src}):`, e);
            }
        }
        return any ? off.startRendering() : null;
    }
    async function buildExportAudio() {
        const items = audioSources();
        if (!items.length) return null;
        const AC = window.AudioContext || window.webkitAudioContext;
        const ctx = new AC();
        const dest = ctx.createMediaStreamDestination();
        const plans = [];
        for (const it of items) {
            try {
                const buf = (window.ResourceSystem && typeof window.ResourceSystem.loadDecodedAudio === 'function')
                    ? await window.ResourceSystem.loadDecodedAudio(it.src, ctx)
                    : await ctx.decodeAudioData(await (await fetch(resolveSrc(it.src))).arrayBuffer());
                plans.push({ it, buf });
            } catch (e) {
                console.warn(`[Timeline] Export audio decode error for "${it.id}" (${it.src}):`, e);
            }
        }
        if (!plans.length) { ctx.close(); return null; }
        let t0 = null;
        let endAt = 0;
        return {
            stream: dest.stream,
            async resume() {
                if (ctx.state !== 'running') { try { await ctx.resume(); } catch (e) { } }
            },
            elapsedMs() { return t0 === null ? 0 : Math.max(0, (ctx.currentTime - t0) * 1000); },
            hasStarted() { return t0 !== null && ctx.currentTime >= t0; },
            remainingMs() { return t0 === null ? 0 : (endAt - ctx.currentTime) * 1000; },
            start(leadSec = 0.25) {
                t0 = ctx.currentTime + leadSec;
                for (const { it, buf } of plans) {
                    const durS = (it.end - it.start) / 1000;
                    const at = t0 + it.start / 1000;
                    const g = ctx.createGain();
                    applyVolumeEnvelope(g.gain, it, at);
                    const src = ctx.createBufferSource();
                    src.buffer = buf;
                    src.connect(g); g.connect(dest);
                    const playS = Math.min(durS, buf.duration);
                    src.start(at, 0, playS);
                    endAt = Math.max(endAt, at + playS);
                }
            },
            close() { try { ctx.close(); } catch (e) { } }
        };
    }
    // ------------------------------------------------------------------------
    // 4. PROJECT INGESTION & C++ BINDING
    // ------------------------------------------------------------------------
    function parseCssHexU32(str) {
        if (!str) return 0xFF000000;
        let s = String(str).trim();
        if (s.startsWith('#')) s = s.slice(1);
        if (s.length === 3) s = s[0] + s[0] + s[1] + s[1] + s[2] + s[2];
        if (s.length === 6) {
            const r = parseInt(s.slice(0, 2), 16) || 0;
            const g = parseInt(s.slice(2, 4), 16) || 0;
            const b = parseInt(s.slice(4, 6), 16) || 0;
            return ((0xFF << 24) | (b << 16) | (g << 8) | r) >>> 0;
        }
        return 0xFF000000;
    }
    function syncNativeBassHits(tl) {
        const bridge = window.WasmBridge;
        if (!bridge || !bridge.position || !Array.isArray(tl)) return;
        for (let slot = 0; slot < tl.length; slot++) {
            const item = tl[slot];
            const inlineBass = item.params && item.params.bass;
            const bass = inlineBass || state.bassEnvelope;
            if (bass && bass.hit && bass.hit.length) {
                bridge.position.setBassHits(slot, bass.hit);
            }
            if (inlineBass && inlineBass.hit && inlineBass.hit.length &&
                (item.effect === 'zoom' || item.effect === 'flicker') &&
                (item.source === 'bass' || (item.params && item.params.source === 'bass'))) {
                bridge.manager.setNativeBassEnvelope(slot, inlineBass);
            }
        }
    }
    // [BYPASS 1]: Direct Wasm buffer ingestion without JSON.parse intermediary
    function loadTimelineProjectBuffer(buffer, stageW, stageH) {
        const rc = state.renderConfig || {};
        const sW = ((Number(stageW || rc.baseW) || (15 << 7)) + 1) & ~1;
        const sH = ((Number(stageH || rc.baseH) || (135 << 3)) + 1) & ~1;
        state.bassEnvelope = null;
        const bridge = window.WasmBridge;
        if (!bridge || !bridge.isReady() || !bridge.manager) {
            throw new Error('[Timeline Fatal] Native EngineManager is not ready.');
        }
        if (global.CanvasCompositor && global.CanvasCompositor.resetProject) {
            global.CanvasCompositor.resetProject();
        }
        const compiled = bridge.manager.loadProjectBuffer(buffer, sW, sH);
        if (!compiled || !Array.isArray(compiled.items)) {
            throw new Error('[Timeline Fatal] Native project compiler failed.');
        }
        state.durationMs = compiled.durationMs;
        bindCompiledTimeline(compiled.items);
        if (global.CanvasCompositor && global.CanvasCompositor.setProjectJson) {
            try {
                const jsonText = new TextDecoder().decode(buffer);
                global.CanvasCompositor.setProjectJson(jsonText, sW, sH);
            } catch (_) {}
        }
        if (!state.templates && state.ram && state.ram.projectJson) { try { const p = JSON.parse(state.ram.projectJson); state.templates = p.template || p.templates || []; } catch(_) {} }
        for (const rec of mediaPool.values()) {
            if (rec && rec.el) {
                try { rec.el.pause(); } catch (e) { }
            }
        }
        mediaPool.clear();
        attachPropertyMasks(state.timeline);
        syncNativeBassHits(state.timeline);
        preloadProjectFonts(state.timeline, state.templates || []);
        if (bridge.ttClear) bridge.ttClear();
        for (const k in dirtyStats) dirtyStats[k] = 0;
        if (bridge.resetTelemetry) bridge.resetTelemetry();
        if (global.DebugInspector && global.DebugInspector.reset) {
            global.DebugInspector.reset();
        }
        if (state.timeline.some(it => (it.params && it.params.source === 'bass') || it.source === 'bass')) {
            prepareBass();
        }
        if (bridge.manager.readNnueCutTelemetry) {
            window.dispatchEvent(new CustomEvent('nnue-cut-move', { detail: bridge.manager.readNnueCutTelemetry() }));
        }
        window.dispatchEvent(new CustomEvent('project-loaded'));
        return {
            timeline: state.timeline,
            durationMs: state.durationMs
        };
    }
    function parseTimelineProject(data, sourceJson) {
        const config = data.config || {};
        state.config = config;
        state.templates = data.template || data.templates || config.template || config.templates || state.templates || [];
        const rc = state.renderConfig || {};
        const stageW = ((Number(rc.baseW) || (15 << 7)) + 1) & ~1;
        const stageH = ((Number(rc.baseH) || (135 << 3)) + 1) & ~1;
        state.bassEnvelope = null;
        const bridge = window.WasmBridge;
        if (!bridge || !bridge.isReady() || !bridge.manager) {
            throw new Error('[Timeline Fatal] Native EngineManager is not ready.');
        }
        if (global.CanvasCompositor && global.CanvasCompositor.resetProject) {
            global.CanvasCompositor.resetProject();
        }
        const json = (data && typeof data === 'object') ? JSON.stringify(data) : (typeof sourceJson === 'string' ? sourceJson : JSON.stringify(data));
        const compiled = bridge.manager.loadProjectJson(json, stageW, stageH);
        if (!compiled || !Array.isArray(compiled.items)) {
            throw new Error('[Timeline Fatal] Native project compiler failed.');
        }
        if (global.CanvasCompositor && global.CanvasCompositor.setProjectJson) {
            global.CanvasCompositor.setProjectJson(json, stageW, stageH);
        }
        state.durationMs = compiled.durationMs;
        bindCompiledTimeline(compiled.items);
        if (global.CanvasCompositor && global.CanvasCompositor.setProjectJson) {
            try {
                const jsonText = new TextDecoder().decode(buffer);
                global.CanvasCompositor.setProjectJson(jsonText, sW, sH);
            } catch (_) {}
        }
        state.timelineMeta = {
            stylesheet: config.stylesheet || '',
            stage: data.stage || config.stage || {},
            animations: data.animations || config.animations || {}
        };
        // Reset and clear mediaPool on new project load
        for (const rec of mediaPool.values()) {
            if (rec && rec.el) {
                try { rec.el.pause(); } catch (e) { }
            }
        }
        mediaPool.clear();
        attachPropertyMasks(state.timeline);
        syncNativeBassHits(state.timeline);
        preloadProjectFonts(state.timeline, state.templates || []);
        if (bridge.ttClear) bridge.ttClear();
        for (const k in dirtyStats) dirtyStats[k] = 0;
        if (bridge.resetTelemetry) bridge.resetTelemetry();
        if (global.DebugInspector && global.DebugInspector.reset) {
            global.DebugInspector.reset();
        }
        if (state.timeline.some(it => (it.params && it.params.source === 'bass') || it.source === 'bass')) {
            prepareBass();
        }
        if (bridge.manager.readNnueCutTelemetry) {
            window.dispatchEvent(new CustomEvent('nnue-cut-move', { detail: bridge.manager.readNnueCutTelemetry() }));
        }
        window.dispatchEvent(new CustomEvent('project-loaded'));
        return {
            config,
            template: data.template || data.templates || [],
            timeline: state.timeline,
            style: data.style || data.roles || data.styles || [],
            meta: state.timelineMeta
        };
    }
    function applyJsonMove(data, sourceJson) {
        const config = data.config || {};
        state.config = config;
        const rc = state.renderConfig || {};
        const stageW = ((Number(rc.baseW) || (15 << 7)) + 1) & ~1;
        const stageH = ((Number(rc.baseH) || (135 << 3)) + 1) & ~1;
        const bridge = window.WasmBridge;
        if (!bridge || !bridge.isReady() || !bridge.manager) {
            return parseTimelineProject(data, sourceJson);
        }
        const json = (data && typeof data === 'object') ? JSON.stringify(data) : (typeof sourceJson === 'string' ? sourceJson : JSON.stringify(data));
        const compiled = (typeof bridge.manager.doJsonMove === 'function')
            ? bridge.manager.doJsonMove(json, stageW, stageH)
            : bridge.manager.loadProjectJson(json, stageW, stageH);
        if (!compiled || !Array.isArray(compiled.items)) {
            throw new Error('[Timeline Fatal] Native project do_edit_move failed.');
        }
        if (global.CanvasCompositor && global.CanvasCompositor.setProjectJson) {
            global.CanvasCompositor.setProjectJson(json, stageW, stageH);
        }
        state.durationMs = compiled.durationMs;
        bindCompiledTimeline(compiled.items);
        if (global.CanvasCompositor && global.CanvasCompositor.setProjectJson) {
            try {
                const jsonText = new TextDecoder().decode(buffer);
                global.CanvasCompositor.setProjectJson(jsonText, sW, sH);
            } catch (_) {}
        }
        state.timelineMeta = {
            stylesheet: config.stylesheet || '',
            stage: data.stage || config.stage || {},
            animations: data.animations || config.animations || {}
        };
        attachPropertyMasks(state.timeline);
        syncNativeBassHits(state.timeline);
        preloadProjectFonts(state.timeline, state.templates || []);
        if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
            global.CanvasCompositor.invalidateScene('DO_JSON_MOVE');
        }
        if (compiled.nnueCut) {
            window.dispatchEvent(new CustomEvent('nnue-cut-move', { detail: compiled.nnueCut }));
        }
        return {
            config,
            template: data.template || data.templates || [],
            timeline: state.timeline,
            style: data.style || data.roles || data.styles || [],
            meta: state.timelineMeta,
            nnueCut: compiled.nnueCut || null
        };
    }

    /**
     * WARM PATH ATOMIC IN-PLACE MUTATION (< 100ns, Zero JSON)
     */
    function mutateSlotReg(slot, regIdx, val) {
        const bridge = window.WasmBridge;
        if (bridge && bridge.position && typeof bridge.position.mutateRegF32 === 'function') {
            bridge.position.mutateRegF32(slot, regIdx, Number(val));
            if (state.timeline && state.timeline[slot]) {
                const item = state.timeline[slot];
                switch (regIdx) {
                    case 0: item.baseX = val; break;
                    case 1: item.baseY = val; break;
                    case 2: item.baseW = val; break;
                    case 3: item.baseH = val; break;
                    case 4: item.rotateDeg = val; break;
                    case 6: item.opacity = val; break;
                    default: break;
                }
            }
            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                global.CanvasCompositor.invalidateScene('WARM_MUTATE');
            }
            return true;
        }
        return false;
    }

    function mutateSlotBit(slot, bitIdx, val) {
        const bridge = window.WasmBridge;
        if (bridge && bridge.position && typeof bridge.position.mutateBit === 'function') {
            bridge.position.mutateBit(slot, bitIdx, val ? 1 : 0);
            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                global.CanvasCompositor.invalidateScene('WARM_MUTATE_BIT');
            }
            return true;
        }
        return false;
    }

    function mutateFxParam(instanceSlot, regIdx, val) {
        const bridge = window.WasmBridge;
        if (bridge && bridge.position && typeof bridge.position.mutateFxParamF32 === 'function') {
            bridge.position.mutateFxParamF32(instanceSlot, regIdx, Number(val));
            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                global.CanvasCompositor.invalidateScene('WARM_FX_MUTATE');
            }
            return true;
        }
        return false;
    }

    function mutateFxBit(instanceSlot, bitIdx, val) {
        const bridge = window.WasmBridge;
        if (bridge && bridge.position && typeof bridge.position.mutateFxBit === 'function') {
            bridge.position.mutateFxBit(instanceSlot, bitIdx, val ? 1 : 0);
            if (global.CanvasCompositor && global.CanvasCompositor.invalidateScene) {
                global.CanvasCompositor.invalidateScene('WARM_FX_MUTATE_BIT');
            }
            return true;
        }
        return false;
    }

    function getDurationMs() {
        if (state.durationMs !== undefined && state.durationMs > 0) return state.durationMs;
        if (window.WasmBridge) {
            if (window.WasmBridge.manager && typeof window.WasmBridge.manager.getProjectDuration === 'function') {
                const d = window.WasmBridge.manager.getProjectDuration();
                if (d > 0) return d;
            }
            if (window.WasmBridge.sharedState && typeof window.WasmBridge.sharedState.getDuration === 'function') {
                const d = window.WasmBridge.sharedState.getDuration();
                if (d > 0) return d;
            }
        }
        return (state.config && state.config.durationMs) || 0;
    }
    // ------------------------------------------------------------------------
    // 5. RESOLUTION & RENDER HOOKS (Clock / rAF Hook)
    // ------------------------------------------------------------------------
    function resolvePreviewResolution(cfg, baseW, baseH, k = 1, dpr = 1) {
        if (typeof global.resolvePreviewResolution === 'function' && global.resolvePreviewResolution !== resolvePreviewResolution) {
            return global.resolvePreviewResolution(cfg, baseW, baseH, k, dpr);
        }
        const rc = state.renderConfig || {};
        const defW = ((baseW || rc.baseW || (15 << 7)) + 1) & ~1;
        const defH = ((baseH || rc.baseH || (135 << 3)) + 1) & ~1;
        return { w: defW, h: defH, source: 'fallback' };
    }
    let lastRenderNowMs = 0;
    function renderTimelineFrame(nowMs, container, opts) {
        if (lastRenderNowMs > 0 && nowMs < lastRenderNowMs && (lastRenderNowMs - nowMs) > 100) {
            for (const k in dirtyStats) dirtyStats[k] = 0;
        }
        lastRenderNowMs = nowMs;
        opts = opts || {};
        const rc = state.renderConfig || {};
        const baseW = opts.baseW || rc.baseW || 1920;
        const baseH = opts.baseH || rc.baseH || 1080;
        const isPlaying = !!state.isPlaying;
        // 1. Browser Hardware Media Synchronization
        syncMedia(nowMs, isPlaying);
        // 2. Resolve Viewport Resolution
        const cfg = state.config || {};
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const res = resolvePreviewResolution(cfg, baseW, baseH, 1, dpr);
        const rw = (res && res.w > 0) ? res.w : (baseW > 0 ? baseW : 1920);
        const rh = (res && res.h > 0) ? res.h : (baseH > 0 ? baseH : 1080);
        // 3. 100% C++ Core Engine Execution via CanvasCompositor
        if (window.CanvasCompositor && window.CanvasCompositor.compositeTimelineFrame) {
            try {
                window.CanvasCompositor.compositeTimelineFrame(
                    stage, null, nowMs, rw, rh, baseW, baseH, null, null
                );
            } catch (err) {
                console.warn('[TimelineEngine] compositeTimelineFrame error:', err);
            }
        }
    }
    const exportFontPreloads = new WeakMap();
    async function drawTimelineFrameAsync(ctx, width, height, nowMs, ec) {
        ec = ec || {};
        const rc = state.renderConfig || {};
        const baseW = ec.baseW || rc.baseW || width;
        const baseH = ec.baseH || rc.baseH || height;
        const items = state.timeline;
        let fontPreload = exportFontPreloads.get(items);
        if (!fontPreload) {
            fontPreload = preloadProjectFonts(items, state.templates || [], false);
            exportFontPreloads.set(items, fontPreload);
        }
        await fontPreload;
        if (window.CanvasCompositor && window.CanvasCompositor.compositeTimelineFrame) {
            await window.CanvasCompositor.compositeTimelineFrame(
                stage, ctx, nowMs, width, height, baseW, baseH, null, null
            );
        }
        return 0; // ready
    }
    function drawTimelineFrame(ctx, width, height, nowMs, ec) {
        drawTimelineFrameAsync(ctx, width, height, nowMs, ec);
    }
    function resetAll() {
        pauseAllMedia();
        mediaPool.clear();
        assetUrls.clear();
        bindCompiledTimeline([]);
        if (window.WasmBridge && window.WasmBridge.ttClear) {
            window.WasmBridge.ttClear();
        }
    }
    // ------------------------------------------------------------------------
    // 6. PUBLIC API EXPORTS
    // ------------------------------------------------------------------------
    const TimelineEngine = {
        resetProject() {
            for (const rec of mediaPool.values()) {
                if (rec && rec.el) {
                    try { rec.el.pause(); rec.el.src = ''; } catch (e) {}
                }
            }
            mediaPool.clear();
            if (typeof assetUrls !== 'undefined' && assetUrls && assetUrls.forEach) {
                assetUrls.forEach(url => {
                    try {
                        if (url && typeof url === 'string' && url.startsWith('blob:')) {
                            URL.revokeObjectURL(url);
                        }
                    } catch (e) {}
                });
                assetUrls.clear();
            }
            bindCompiledTimeline([]);
            state.timelineMeta = {};
            state.durationMs = 0;
            state.bassEnvelope = null;
        },
        loadProjectBuffer: loadTimelineProjectBuffer,
        parseProject: parseTimelineProject,
        resetStageCache: () => {},
        renderFrame: renderTimelineFrame,
        drawFrame: drawTimelineFrame,
        drawFrameAsync: drawTimelineFrameAsync,
        drawTextFrame: drawTimelineFrame,
        disposeExport: () => {},
        applyJsonMove,
        mutateSlotReg,
        mutateSlotBit,
        mutateFxParam,
        mutateFxBit,
        preloadAssets,
        prepareBass,
        buildExportAudio,
        buildWaveformBuffer,
        buildHighResAudioBuffer,
        audioSources,
        getDurationMs,
        registerAssetBlob,
        unregisterAssetBlob,
        getAssetBlob,
        getFxState: () => ({}),
        FX_BIT: {},
        reset: resetAll,
        resolvePreviewResolution,
        getPreviewResolution: (cfg) => {
            const rc = state.renderConfig || {};
            return resolvePreviewResolution(cfg || state.config, rc.baseW || 1920, rc.baseH || 1080, 1, 1);
        },
        isGLEffect: () => true,
        dirtyStats,
        computeFrameZobrist: () => 0n,
        pauseMedia: pauseAllMedia,
        setPlaybackRate: setAllPlaybackRate,
        getTimelineItem: (slot) => (state.timeline && state.timeline[slot]) || null,
        hasTimelineItems: () => Array.isArray(state.timeline) && state.timeline.length > 0,
        getStage: () => stage,
        getExportStage: () => stage,
        resolveSrc,
        syncMedia,
        unlockAudio,
        preloadProjectFonts,
        loadProject: parseTimelineProject,
        parseProject: parseTimelineProject,
        getPlaybackMediaTimeMs,
        evaluateAudioItemVolume,
        getAudioEnvelopePoints,
        applyVolumeEnvelope,
        mediaPool,
        options: OPT,
        PropertyBit,
        PropertyModule: (typeof PropertyModule !== 'undefined' ? PropertyModule : null),
        PropertyBitmask: (typeof PropertyModule !== 'undefined' ? PropertyModule.PropertyBitmask : null),
        PropertyRegistry: (typeof PropertyModule !== 'undefined' ? PropertyModule.PropertyRegistry : null),
        computeItemPropertyMasks,
        countBits64,
        attachPropertyMasks,
        isPureStatic,
        isOpacityOnlyVariance,
        hasSpatialVariance,
        refreshFx: () => {}
    };
    global.renderTimelineFrame = renderTimelineFrame;
    global.parseTimelineProject = parseTimelineProject;
    global.TimelineEngine = TimelineEngine;
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = TimelineEngine;
    }
})();
