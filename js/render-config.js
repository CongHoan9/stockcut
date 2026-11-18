// ============================================================================
// RENDER CONFIG — tính MỘT lần khi config đổi (không tính mỗi frame)
//   Phụ thuộc : ui.js, state.js, fonts.js
//   Cung cấp  : buildRenderConfig
// ============================================================================
// ============================================================
// [OPT-2/3/4] Pre-calculate RenderConfig & Function Pointers
// Gọi MỘT LẦN duy nhất khi config thay đổi, KHÔNG gọi mỗi frame
// ============================================================
async function buildRenderConfig(projectData = null) {
    const data = projectData || (state.ram && state.ram.projectJson ? (() => { try { return JSON.parse(state.ram.projectJson); } catch(_) { return null; } })() : null);
    const cfg = (data && data.config) || state.config || {};
    const ratioStr = cfg.aspectRatio || "original";
    let ratioW = 16, ratioH = 9;
    if (ratioStr === "original") {
        let foundMedia = false;
        const items = (data && data.timeline) || (window.state && window.state.timeline) || [];
        for (const it of items) {
            const w = it.videoWidth || it.naturalWidth || it.w || it.width || it.baseW;
            const h = it.videoHeight || it.naturalHeight || it.h || it.height || it.baseH;
            if (w > 0 && h > 0) {
                ratioW = w;
                ratioH = h;
                foundMedia = true;
                break;
            }
        }
        if (!foundMedia) {
            ratioW = 16;
            ratioH = 9;
        }
    } else {
        const rp = String(ratioStr).split(':');
        ratioW = parseFloat(rp[0]) || 16;
        ratioH = parseFloat(rp[1]) || 9;
    }
    let baseW = parseInt(cfg.width || cfg.stageWidth, 10);
    let baseH = parseInt(cfg.height || cfg.stageHeight, 10);
    if (!(baseW > 0 && baseH > 0)) {
        const minAxis = Math.min(ratioW, ratioH);
        const unitSpan = (minAxis >>> 7) ? minAxis : ((1 << 10) | (7 << 3));
        const scale = unitSpan / (minAxis || 1);
        baseW = (Math.round(ratioW * scale) + 1) & ~1;
        baseH = (Math.round(ratioH * scale) + 1) & ~1;
    } else {
        baseW = (baseW + 1) & ~1;
        baseH = (baseH + 1) & ~1;
    }
    cfg.baseW = baseW;
    cfg.baseH = baseH;
    const lyricSize = cfg.lyricSize !== undefined ? parseFloat(cfg.lyricSize) || 60 : 60;
    const defaultFamily = (typeof window !== 'undefined' && window.NativeFontAssets && typeof window.NativeFontAssets.getDefaultFamily === 'function')
        ? window.NativeFontAssets.getDefaultFamily()
        : 'sans-serif';
    const fontFamily = cfg.fontFamily || cfg['font-family'] || defaultFamily;
    state.renderConfig = {
        ratioW, ratioH, baseW, baseH,
        lyricSize,
        lyricFontWeight: String(cfg.lyricFontWeight || "700"),
        fontFamily,
        fadeOutEnabled: cfg.fadeOut === true,
        fadeOutDuration: typeof cfg.fadeOutDuration === 'number' ? cfg.fadeOutDuration : 2000
    };
    if (typeof window !== 'undefined' && window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.position && window.WasmBridge.position.setStageDimensions) {
        window.WasmBridge.position.setStageDimensions(baseW, baseH);
    }
    if (typeof window !== 'undefined' && window.TimelineEngine && window.TimelineEngine.resetStageCache) {
        window.TimelineEngine.resetStageCache();
    }
    return state.renderConfig;
}
// ============================================================================
// [MODULE DÙNG CHUNG]: TÍNH ĐỘ PHÂN GIẢI PREVIEW TỪ CONFIG (previewResolution)
// Quy định độ sắc nét của Framebuffer vật lý, hoàn toàn độc lập với thước đo thiết kế baseW x baseH.
// ============================================================================
function resolvePreviewResolution(cfg, baseW, baseH, k = 1, dpr = 1) {
    const rc = (typeof state !== 'undefined' && state && state.renderConfig) ? state.renderConfig : null;
    const bW = ((baseW > 0 ? baseW : (rc && rc.baseW > 0 ? rc.baseW : (15 << 7))) + 1) & ~1;
    const bH = ((baseH > 0 ? baseH : (rc && rc.baseH > 0 ? rc.baseH : (135 << 3))) + 1) & ~1;
    const minBase = Math.min(bW, bH) || 1;
    const raw = (typeof cfg === 'string' || typeof cfg === 'number' || Array.isArray(cfg))
        ? cfg
        : (cfg ? (cfg.previewResolution !== undefined ? cfg.previewResolution :
            (cfg.previewRes !== undefined ? cfg.previewRes :
                (cfg['preview-resolution'] !== undefined ? cfg['preview-resolution'] :
                    (cfg.preview_resolution !== undefined ? cfg.preview_resolution : cfg.previewHeight)))) : undefined);
    // 1. Mặc định khi không khai báo previewResolution: độ phân giải gốc bW x bH
    if (raw === undefined || raw === null || raw === '') {
        return {
            w: bW,
            h: bH,
            source: 'default_base'
        };
    }
    // 2. Mảng [W, H]
    if (Array.isArray(raw) && raw.length >= 2) {
        const rw = parseInt(raw[0], 10) || bW;
        const rh = parseInt(raw[1], 10) || bH;
        return {
            w: (rw + 1) & ~1,
            h: (rh + 1) & ~1,
            source: 'array'
        };
    }
    // 3. Số: tỷ lệ phóng đại (0 < raw <= 2) hoặc độ phân giải trục ngắn
    if (typeof raw === 'number' && Number.isFinite(raw)) {
        const scale = (raw > 0 && raw <= 2)
            ? raw
            : (Math.max(120, Math.min(135 << 5, Math.round(raw))) / minBase);
        return {
            w: Math.max(2, (Math.round(bW * scale) + 1) & ~1),
            h: Math.max(2, (Math.round(bH * scale) + 1) & ~1),
            source: (raw > 0 && raw <= 2) ? 'scale' : 'target_height'
        };
    }
    // 4. Chuỗi
    if (typeof raw === 'string') {
        const str = raw.trim().toLowerCase();
        if (str === 'full' || str === 'original' || str === 'native' || str === 'max' || str === '100%') {
            return {
                w: bW,
                h: bH,
                source: 'original'
            };
        }
        if (str === 'auto' || str === 'viewport' || str === 'fit') {
            const factor = (k || 1) * (dpr || 1);
            return {
                w: Math.max(2, (Math.round(bW * factor) + 1) & ~1),
                h: Math.max(2, (Math.round(bH * factor) + 1) & ~1),
                source: 'auto_viewport'
            };
        }
        if (str.endsWith('%')) {
            const pct = parseFloat(str) * 0.01;
            if (Number.isFinite(pct) && pct > 0) {
                return {
                    w: Math.max(2, (Math.round(bW * pct) + 1) & ~1),
                    h: Math.max(2, (Math.round(bH * pct) + 1) & ~1),
                    source: 'percentage'
                };
            }
        }
        const dimMatch = str.match(/^(\d+)\s*[xX*]\s*(\d+)$/);
        if (dimMatch) {
            const rw = parseInt(dimMatch[1], 10);
            const rh = parseInt(dimMatch[2], 10);
            if (rw > 0 && rh > 0) {
                return {
                    w: (rw + 1) & ~1,
                    h: (rh + 1) & ~1,
                    source: 'explicit_dim'
                };
            }
        }
        let targetSpan = 0;
        if (str === '4k' || str === 'uhd') targetSpan = 135 << 4;
        else if (str === '2k' || str === 'qhd') targetSpan = 90 << 4;
        else if (str === 'fhd') targetSpan = 135 << 3;
        else if (str === 'hd') targetSpan = 45 << 4;
        else if (str === 'sd') targetSpan = 15 << 5;
        else {
            const parsed = parseInt(str, 10);
            if (Number.isFinite(parsed) && parsed > 0) targetSpan = parsed;
        }
        if (targetSpan > 0) {
            const scale = targetSpan / minBase;
            return {
                w: Math.max(2, (Math.round(bW * scale) + 1) & ~1),
                h: Math.max(2, (Math.round(bH * scale) + 1) & ~1),
                source: 'named_preset'
            };
        }
    }
    return {
        w: bW,
        h: bH,
        source: 'fallback'
    };
}
// ============================================================================
// [MODULE DÙNG CHUNG]: TÍNH PADDING KHUNG CHỮ (CHỐNG CẮT SCISSOR & BOUNDING BOX)
// Đảm bảo đồng bộ 100% giữa WebGL Text Quad và C++ SlotStateTracker Dirty Rect
// ============================================================================
function computeTextPadding(fontSize, dw = 0, effH = 0) {
    const pad = Math.max(128, Math.ceil(fontSize * 1.5));
    const padX = Math.min(pad, Math.max(0, Math.floor((4096 - dw) * 0.5)));
    const padY = Math.min(pad, Math.max(0, Math.floor((2048 - effH) * 0.5)));
    return { padX, padY };
}
// Export cho toàn hệ thống
if (typeof window !== 'undefined') {
    window.resolvePreviewResolution = resolvePreviewResolution;
    window.computeTextPadding = computeTextPadding;
}
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { buildRenderConfig, resolvePreviewResolution, computeTextPadding };
}
