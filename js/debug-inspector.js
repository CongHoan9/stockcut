// ============================================================================
// js/debug-inspector.js — CONTROLLER CHO BẢNG DEBUG INSPECTOR THEME VS CODE TÍM
// - Tương tác: KÉO CHUỘT (ns-resize) ở mép trên, hoặc CLICK 1 LẦN vào status bar để
//   thu/phóng drawer (phóng to hết cỡ <-> trở về chiều cao trước đó).
// - Cập nhật giao diện: Throttled ở 10Hz (100ms/lần), ZERO ALLOCATION trong render loop.
// ============================================================================
(function () {
    'use strict';
    // DOM Elements Cache
    let panelEl, resizerEl, drawerEl, statusBarEl, chevronEl;
    let chipFpsText, chipFpsNum, chipMsText, chipFpsDot, chipTimeText, chipFrameText, chipModeText, chipFxText, chipLayersText, chipSizeText, chipScaleText;
    let cardFpsVal, cardFpsDot, cardFrameTimeVal, cardCpuTimeVal, cardTimeVal, cardFrameVal, cardEpochVal, cardVerVal, cardResVal, cardCanvasResVal, cardScaleVal, cardDprTag;
    let cardModePill, cardModeDetail, cardModeStats;
    let tabFxCount, tabLayersCount, effectsTbody, layersTbody;
    let zobristHashVal, ttHitsVal, ttMissesVal, ttBreakerVal, statFullVal, statDirtyVal, statNoneVal;
    let cacheHitRateVal;
    let commandCacheStateVal, commandCacheDetailVal, pixelRenderStateVal, pixelRenderDetailVal;
    let tabBtns = [], tabPanes = {};
    let isDragging = false;
    let dragStartY = 0;
    let dragStartHeight = 0;
    let currentDrawerHeight = 0;
    // Chiều cao drawer được nhớ (lần kéo cuối), dùng khi mở lại bằng click; lưu cả qua lần tải lại trang
    const HEIGHT_STORE_KEY = 'debugDrawerHeight';
    const DEFAULT_OPEN_HEIGHT = 220;
    const MIN_OPEN_HEIGHT = 220;
    let savedHeight = loadSavedHeight();
    let dragSetupDone = false;    // initDOMElements() có thể chạy nhiều lần -> chỉ gắn listener 1 lần
    let lastDomUpdate = 0;
    let activeTabId = 'telemetry';
    // Circular Buffer Zero-Allocation cho đồ thị FPS biến động
    const FPS_HISTORY_CAP = 60;
    const fpsHistory = new Float32Array(FPS_HISTORY_CAP).fill(60.0);
    let fpsHistoryIdx = 0;
    let lastTelemNowMs = -1;
    let fpsCanvas, fpsCanvasCtx;
    let fpsMinVal, fpsAvgVal, fpsMaxVal, budgetTextVal, budgetMeterBar, budgetPctVal, playbackSpeedVal, aspectVal;
    // Format mm:ss.mmm từ milliseconds
    function formatTimeMs(ms) {
        if (!Number.isFinite(ms) || ms < 0) ms = 0;
        const totalSec = ms / 1000;
        const mins = Math.floor(totalSec / 60);
        const secs = Math.floor(totalSec % 60);
        const millis = Math.floor(ms % 1000);
        return `${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}.${String(millis).padStart(3, '0')}`;
    }
    // Khởi tạo các phần tử DOM
    function initDOMElements() {
        panelEl = document.getElementById('debug-inspector-panel');
        resizerEl = document.getElementById('debug-resizer');
        drawerEl = document.getElementById('debug-drawer-body');
        statusBarEl = document.getElementById('debug-status-bar');
        chevronEl = document.getElementById('debug-chevron');
        // Status bar chips
        chipFpsText = document.getElementById('chip-fps-text');
        chipFpsNum = document.getElementById('chip-fps-num');
        chipMsText = document.getElementById('chip-ms-text');
        chipFpsDot = document.getElementById('chip-fps-dot');
        chipTimeText = document.getElementById('chip-time-text');
        chipFrameText = document.getElementById('chip-frame-text');
        chipModeText = document.getElementById('chip-mode-text');
        chipFxText = document.getElementById('chip-fx-text');
        chipLayersText = document.getElementById('chip-layers-text');
        chipSizeText = document.getElementById('chip-size-text');
        chipScaleText = document.getElementById('chip-scale-text');
        // Card metrics
        cardFpsVal = document.getElementById('dbg-fps-val');
        cardFpsDot = document.getElementById('dbg-fps-status-dot');
        cardFrameTimeVal = document.getElementById('dbg-frametime-val');
        cardCpuTimeVal = document.getElementById('dbg-cputime-val');
        cardTimeVal = document.getElementById('dbg-time-val');
        cardFrameVal = document.getElementById('dbg-frame-val');
        cardModePill = document.getElementById('dbg-mode-pill');
        cardModeDetail = document.getElementById('dbg-mode-detail');
        cardModeStats = document.getElementById('dbg-mode-stats');
        cardEpochVal = document.getElementById('dbg-epoch-val');
        cardVerVal = document.getElementById('dbg-ver-val');
        cardResVal = document.getElementById('dbg-res-val');
        cardCanvasResVal = document.getElementById('dbg-canvasres-val');
        cardScaleVal = document.getElementById('dbg-scale-val');
        cardDprTag = document.getElementById('dbg-dpr-tag');
        // FPS Realtime Graph & Dashboard
        fpsCanvas = document.getElementById('dbg-fps-canvas');
        if (fpsCanvas) {
            fpsCanvasCtx = fpsCanvas.getContext('2d');
        }
        fpsMinVal = document.getElementById('dbg-fps-min');
        fpsAvgVal = document.getElementById('dbg-fps-avg');
        fpsMaxVal = document.getElementById('dbg-fps-max');
        budgetTextVal = document.getElementById('dbg-budget-text');
        budgetMeterBar = document.getElementById('dbg-budget-meter');
        budgetPctVal = document.getElementById('dbg-budget-pct');
        playbackSpeedVal = document.getElementById('dbg-playback-speed');
        aspectVal = document.getElementById('dbg-aspect-val');
        // Tabs & Counts
        tabFxCount = document.getElementById('dbg-tab-fx-count');
        tabLayersCount = document.getElementById('dbg-tab-layers-count');
        effectsTbody = document.getElementById('dbg-effects-tbody');
        layersTbody = document.getElementById('dbg-layers-tbody');
        // Cache stats
        zobristHashVal = document.getElementById('dbg-zobrist-hash');
        ttHitsVal = document.getElementById('dbg-tt-hits');
        ttMissesVal = document.getElementById('dbg-tt-misses');
        ttBreakerVal = document.getElementById('dbg-tt-breaker');
        cacheHitRateVal = document.getElementById('dbg-cache-hit-rate');
        statFullVal = document.getElementById('dbg-stat-full');
        statDirtyVal = document.getElementById('dbg-stat-dirty');
        statNoneVal = document.getElementById('dbg-stat-none');
        commandCacheStateVal = document.getElementById('dbg-command-cache-state');
        commandCacheDetailVal = document.getElementById('dbg-command-cache-detail');
        pixelRenderStateVal = document.getElementById('dbg-pixel-render-state');
        pixelRenderDetailVal = document.getElementById('dbg-pixel-render-detail');
        // Tab buttons & panes
        tabBtns = Array.from(document.querySelectorAll('.debug-tab-btn, .debug-tab-item'));
        tabBtns.forEach(btn => {
            const tabId = btn.getAttribute('data-tab');
            const pane = document.getElementById(`debug-pane-${tabId}`);
            if (pane) tabPanes[tabId] = pane;
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                switchTab(tabId);
            });
        });
        setupDragInteraction();
    }
    // Chuyển tab trong drawer
    function switchTab(tabId) {
        activeTabId = tabId;
        window.__debugActiveTab = tabId;
        tabBtns.forEach(b => {
            if (b.getAttribute('data-tab') === tabId) b.classList.add('active');
            else b.classList.remove('active');
        });
        for (const [id, pane] of Object.entries(tabPanes)) {
            if (id === tabId) pane.classList.add('active');
            else pane.classList.remove('active');
        }
        updateTelemetryDOM(performance.now(), true);
    }
    // ========================================================================
    // TƯƠNG TÁC KÉO CHUỘT (DRAG-TO-RESIZE)
    // Kéo ở mép trên (resizer) hoặc click 1 lần vào status bar để thu/phóng.
    // ========================================================================
    function setupDragInteraction() {
        if (!resizerEl || !drawerEl) return;
        // Script nằm cuối trang: initDOMElements() chạy ngay và chạy lại ở DOMContentLoaded.
        // Không có guard này, listener click bị gắn 2 lần => mở rồi đóng ngay trong cùng 1 click.
        if (dragSetupDone) return;
        dragSetupDone = true;
        // Chevron chỉ là nhãn: không nhận chuột, click xuyên xuống status bar
        if (chevronEl) {
            chevronEl.style.pointerEvents = 'none';
            chevronEl.style.userSelect = 'none';
            chevronEl.style.cursor = 'default';
        }
        // Click 1 lần vào status bar: thu/phóng drawer
        // (con bên trong có pointer-events: none nên click luôn trúng thẳng status bar)
        if (statusBarEl) {
            statusBarEl.addEventListener('click', (e) => {
                e.preventDefault();
                toggleDrawer();
            });
        }
        // Cửa sổ đổi kích thước: drawer luôn nằm trong vùng cho phép.
        // Co lại theo vùng trống, nhưng KHÔNG ghi đè chiều cao đã nhớ (nới ra lại thì mở lại đúng cỡ cũ)
        window.addEventListener('resize', () => {
            if (currentDrawerHeight <= 0) return;
            const maxH = getMaxDrawerHeight();
            const target = clampOpenHeight(savedHeight, maxH);
            if (target !== currentDrawerHeight) setDrawerHeight(target, false);
        });
        // Bắt đầu kéo khi mousedown vào thanh resizer ở mép trên
        resizerEl.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return; // Chỉ nhận chuột trái
            isDragging = true;
            dragStartY = e.clientY;
            dragStartHeight = currentDrawerHeight;
            resizerEl.classList.add('is-dragging');
            document.body.style.cursor = 'ns-resize';
            document.body.style.userSelect = 'none';
            e.preventDefault();
            e.stopPropagation();
        });
        // Di chuyển chuột toàn trang
        window.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            // Kéo lên trên (e.clientY < dragStartY) => chiều cao tăng
            const deltaY = dragStartY - e.clientY;
            let targetHeight = dragStartHeight + deltaY;
            // Giới hạn chiều cao: 0px đến vùng trống giữa header trên và status bar. Dưới 24px thì đóng hẳn
            const maxH = getMaxDrawerHeight();
            if (targetHeight < 24) {
                targetHeight = 0;
            } else if (targetHeight > maxH) {
                targetHeight = maxH;
            }
            setDrawerHeight(targetHeight);
        }, { passive: true });
        // Kết thúc kéo chuột
        window.addEventListener('mouseup', () => {
            if (!isDragging) return;
            isDragging = false;
            if (resizerEl) resizerEl.classList.remove('is-dragging');
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
            persistSavedHeight();
            // Cập nhật lại khung render video viewport sau khi đổi chiều cao
            if (typeof refreshViewport === 'function') {
                refreshViewport();
            }
        });
        // Touch support cho màn hình cảm ứng
        resizerEl.addEventListener('touchstart', (e) => {
            if (e.touches.length !== 1) return;
            isDragging = true;
            dragStartY = e.touches[0].clientY;
            dragStartHeight = currentDrawerHeight;
            resizerEl.classList.add('is-dragging');
            e.preventDefault();
        }, { passive: false });
        window.addEventListener('touchmove', (e) => {
            if (!isDragging || e.touches.length !== 1) return;
            const deltaY = dragStartY - e.touches[0].clientY;
            let targetHeight = dragStartHeight + deltaY;
            const maxH = getMaxDrawerHeight();
            if (targetHeight < 24) targetHeight = 0;
            else if (targetHeight > maxH) targetHeight = maxH;
            setDrawerHeight(targetHeight);
        }, { passive: true });
        window.addEventListener('touchend', () => {
            if (!isDragging) return;
            isDragging = false;
            if (resizerEl) resizerEl.classList.remove('is-dragging');
            persistSavedHeight();
            if (typeof refreshViewport === 'function') refreshViewport();
        });
    }
    // Chiều cao tối đa = khoảng trống giữa đáy header trên (.controls-panel) và đỉnh status bar.
    // Status bar không di chuyển khi drawer mở (drawer là overlay) nên đo trực tiếp được.
    function getMaxDrawerHeight() {
        const head = document.querySelector('.studio-header') || document.querySelector('.controls-panel');
        if (statusBarEl && head) {
            const h = Math.floor(statusBarEl.getBoundingClientRect().top - head.getBoundingClientRect().bottom);
            if (h > 0) return h;
        }
        return 0; // fallback khi không đo được
    }
    function loadSavedHeight() {
        try {
            const v = parseFloat(localStorage.getItem(HEIGHT_STORE_KEY));
            if (isFinite(v) && v >= MIN_OPEN_HEIGHT) return v;
        } catch (e) { }
        return DEFAULT_OPEN_HEIGHT;
    }
    function persistSavedHeight() {
        try { localStorage.setItem(HEIGHT_STORE_KEY, String(Math.round(savedHeight))); } catch (e) { }
    }
    // Luôn nằm trong [MIN_OPEN_HEIGHT, maxH] (maxH = khoảng giữa header trên và status bar)
    function clampOpenHeight(h, maxH) {
        const hi = Math.max(maxH, 0);
        return Math.min(Math.max(h, Math.min(MIN_OPEN_HEIGHT, hi)), hi);
    }
    // Click status bar: đang mở -> đóng hẳn (height 0); đang đóng -> mở lại đúng chiều cao đã nhớ
    function toggleDrawer() {
        if (currentDrawerHeight > 0) {
            setDrawerHeight(0);
        } else {
            setDrawerHeight(clampOpenHeight(savedHeight, getMaxDrawerHeight()), false);
        }
        // Drawer là overlay, không đổi kích thước viewport => không gọi refreshViewport (tránh render lại, nhảy FPS)
    }
    // Đặt chiều cao cho drawer (con bên trong bành trướng theo chiều Y, cuộn tự nhiên)
    // remember=false: chỉ đặt tạm (mở lại / co theo cửa sổ), không ghi đè chiều cao đã nhớ
    function setDrawerHeight(heightPx, remember) {
        if (!drawerEl) initDOMElements();
        if (!drawerEl) return;
        currentDrawerHeight = heightPx;
        if (heightPx > 0 && remember !== false) savedHeight = heightPx;
        drawerEl.style.height = heightPx + 'px';
        if (panelEl) {
            panelEl.style.setProperty('--dbg-h', heightPx + 'px');
        }
        if (heightPx > 0) {
            drawerEl.classList.add('is-open');
            window.__debugInspectorOpen = true;
            if (chevronEl) chevronEl.classList.add('is-expanded');
        } else {
            drawerEl.classList.remove('is-open');
            window.__debugInspectorOpen = false;
            if (chevronEl) chevronEl.classList.remove('is-expanded');
        }
    }
    // ========================================================================
    // ZERO-ALLOCATION DOM TELEMETRY LOOP (~10Hz)
    // Đọc số liệu từ Địa chỉ bộ nhớ dùng chung (Wasm Linear Memory / TypedArray)
    // DOM chỉ đọc (read-only), không chỉnh sửa, throttled 100ms
    // ========================================================================
    const sharedTelem = {
        fps: 60,
        frameTimeMs: 16.6,
        cpuTimeMs: 0,
        nowMs: 0,
        currentHash: 0n,
        currentHashHex: '0x0000000000000000',
        frameIndex: 0,
        renderMode: 'DIRTY',
        commandCacheMode: 'MISS',
        fullReason: '',
        dirtyN: 0,
        culledCount: 0,
        activeFxCount: 0,
        activeLayersCount: 0,
        styleEpoch: 0,
        fontsVer: 0,
        mediaVer: 0,
        baseW: 0,
        baseH: 0,
        previewW: 0,
        previewH: 0,
        scale: 1,
        dpr: 1,
        ttHits: 0,
        ttMisses: 0,
        zobristBreakers: 0,
        dirtyAreaPx: 0,
        isPlaying: false,
        dirtyStats: null,
        activeFxList: null,
        activeLayersList: null
    };
    function updateTelemetryDOM(now, force) {
        requestAnimationFrame(updateTelemetryDOM);
        // Throttle 10Hz (100ms/lần): triệt tiêu 100% hiện tượng sụt giảm FPS render
        if (!force && (now - lastDomUpdate < 100)) return;
        lastDomUpdate = now;
        // Đọc trực tiếp từ Địa chỉ bộ nhớ dùng chung giữa Core và DOM (Ưu tiên SharedArrayBuffer nếu RenderWorker đang chạy)
        let telem = null;
        if (window.CanvasCompositor && window.CanvasCompositor.WorkerPresenter && window.CanvasCompositor.WorkerPresenter.enabled) {
            telem = window.CanvasCompositor.WorkerPresenter.getTelemetry(sharedTelem);
        }
        if (!telem && window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.readTelemetry) {
            telem = window.WasmBridge.readTelemetry(sharedTelem);
        }
        if (!telem) {
            telem = window.__timelineTelemetry;
        }
        if (!telem) return;
        // Bổ sung các danh sách chi tiết từ timeline telemetry và C++ shared memory
        if (window.__timelineTelemetry) {
            telem.activeFxList = window.__timelineTelemetry.activeFxList;
            telem.activeLayersList = window.__timelineTelemetry.activeLayersList;
            telem.dirtyStats = window.__timelineTelemetry.dirtyStats;
            telem.commandCacheMode = window.__timelineTelemetry.commandCacheMode;
            telem.fullReason = window.__timelineTelemetry.fullReason;
        }
        if (window.WasmBridge) {
            if (window.WasmBridge.getActiveLayers) {
                telem.activeLayersList = window.WasmBridge.getActiveLayers();
            }
            if (window.WasmBridge.getActiveEffects) {
                telem.activeFxList = window.WasmBridge.getActiveEffects();
            }
        }
        if (telem.activeLayersList && telem.activeLayersList.length >= 0) {
            telem.activeLayersCount = telem.activeLayersList.length;
        }
        if (telem.activeFxList && telem.activeFxList.length >= 0) {
            telem.activeFxCount = telem.activeFxList.length;
        }
        // 1. Cập nhật Status Bar Chips (luôn hiển thị trên waveform)
        const fpsVal = telem.fps || 60;
        const fpsColor = fpsVal >= 50 ? '#10b981' : (fpsVal >= 30 ? '#f59e0b' : '#ef4444');
        if (chipFpsNum) {
            chipFpsNum.textContent = fpsVal.toFixed(1);
            chipFpsNum.style.color = fpsColor;
        } else if (chipFpsText) {
            chipFpsText.innerHTML = `<b style="color: ${fpsColor}; font-weight: 600;">${fpsVal.toFixed(1)}</b> FPS`;
        }
        if (chipMsText) chipMsText.textContent = `(${(telem.frameTimeMs || 16.6).toFixed(1)}ms)`;
        if (chipFpsDot) {
            chipFpsDot.style.display = 'none';
        }
        if (chipTimeText) chipTimeText.textContent = formatTimeMs(telem.nowMs || 0);
        if (chipFrameText) chipFrameText.textContent = `F${telem.frameIndex || 0}`;
        if (chevronEl) {
            const isWorker = !!(typeof window !== 'undefined' &&
                                window.CanvasCompositor &&
                                window.CanvasCompositor.WorkerPresenter &&
                                window.CanvasCompositor.WorkerPresenter.enabled &&
                                window.CanvasCompositor.WorkerPresenter.ready);
            const targetText = isWorker ? 'WORKER' : 'MAIN';
            if (chevronEl.textContent !== targetText) {
                chevronEl.textContent = targetText;
                chevronEl.title = isWorker ? 'Render Context: Dedicated Web Worker' : 'Render Context: Main Thread';
                if (isWorker) {
                    chevronEl.classList.add('is-worker');
                    chevronEl.classList.remove('is-main');
                } else {
                    chevronEl.classList.add('is-main');
                    chevronEl.classList.remove('is-worker');
                }
            }
        }
        if (chipModeText) {
            const m = telem.renderMode || 'DIRTY';
            const modeLabel = m === 'FULL'
                ? 'FULL'
                : (m === 'ZOBRIST_SKIP' ? 'O(1)' : (m === 'NONE' ? 'NONE' : 'DIRTY'));
            chipModeText.textContent = modeLabel;
            const modeClass = m === 'FULL'
                ? 'full'
                : (m === 'ZOBRIST_SKIP' ? 'breaker' : (m === 'NONE' ? 'none' : 'dirty'));
            chipModeText.className = `debug-mode-text ${modeClass}`;
        }
        if (chipFxText) chipFxText.textContent = String(telem.activeFxCount || 0);
        if (chipLayersText) chipLayersText.textContent = String(telem.activeLayersCount || 0);
        const rc = (typeof window !== 'undefined' && window.state && window.state.renderConfig) || {};
        const activePreviewRes = (typeof window !== 'undefined' && window.TimelineEngine && window.TimelineEngine.getPreviewResolution) ? window.TimelineEngine.getPreviewResolution() : null;
        const dispW = telem.baseW || (activePreviewRes && activePreviewRes.w) || rc.baseW || 0;
        const dispH = telem.baseH || (activePreviewRes && activePreviewRes.h) || rc.baseH || 0;
        if (chipSizeText) chipSizeText.textContent = `${dispW}×${dispH}`;
        if (chipScaleText) chipScaleText.textContent = `${(telem.scale || 1).toFixed(2)}x`;
        // 2. Cập nhật buffer biến động FPS & Dashboard (luôn ghi nhận, chỉ vẽ khi Drawer mở)
        const shouldRenderCanvas = currentDrawerHeight > 0 && activeTabId === 'telemetry';
        updateFpsGraph(fpsVal, telem.cpuTimeMs || 0, telem, shouldRenderCanvas);
        // 3. Nếu Drawer đang mở (currentDrawerHeight > 0), cập nhật chi tiết các tab
        if (currentDrawerHeight > 0) {
            updateDrawerPanes(telem);
        }
    }
    let histMinFps = Infinity;
    let histMaxFps = 0.0;
    let histFpsSum = 0.0;
    let histFpsCount = 0;
    function resetHistoricalFps() {
        histMinFps = Infinity;
        histMaxFps = 0.0;
        histFpsSum = 0.0;
        histFpsCount = 0;
        fpsHistory.fill(0);
        fpsHistoryIdx = 0;
    }
    function resetProjectInspection() {
        resetHistoricalFps();
        lastTelemNowMs = 0;
        lastEffectsSig = '';
        lastLayersSig = '';
        if (effectsTbody) effectsTbody.innerHTML = '';
        if (layersTbody) layersTbody.innerHTML = '';
        sharedTelem.activeLayersList = null;
        sharedTelem.activeFxList = null;
        // Xóa sạch bộ nhớ telemetry C++ Native
        if (window.WasmBridge && window.WasmBridge.resetTelemetry) {
            window.WasmBridge.resetTelemetry();
        }
        // Xóa sạch dirtyStats JS
        if (window.TimelineEngine && window.TimelineEngine.dirtyStats) {
            for (const k in window.TimelineEngine.dirtyStats) window.TimelineEngine.dirtyStats[k] = 0;
        }
        if (window.__timelineTelemetry && window.__timelineTelemetry.dirtyStats) {
            for (const k in window.__timelineTelemetry.dirtyStats) window.__timelineTelemetry.dirtyStats[k] = 0;
        }
        // Xóa sạch canvas đồ thị FPS
        if (fpsCanvasCtx && fpsCanvas) {
            fpsCanvasCtx.clearRect(0, 0, fpsCanvas.width, fpsCanvas.height);
        }
        // Đặt lại các số liệu min, avg, max về trạng thái chờ
        if (fpsMinVal) fpsMinVal.textContent = '--';
        if (fpsAvgVal) fpsAvgVal.textContent = '--';
        if (fpsMaxVal) fpsMaxVal.textContent = '--';
        if (budgetTextVal) budgetTextVal.textContent = '(0.0%)';
        if (budgetPctVal) budgetPctVal.textContent = '0.0%';
        if (budgetMeterBar) budgetMeterBar.style.width = '0%';
        if (cardCpuTimeVal) cardCpuTimeVal.textContent = '0.00';
        if (chipTimeText) chipTimeText.textContent = '00:00.000';
        if (cardTimeVal) cardTimeVal.textContent = '00:00.000';
        if (chipFrameText) chipFrameText.textContent = 'F0';
        if (cardFrameVal) cardFrameVal.textContent = 'F0';
        updateTelemetryDOM(performance.now(), true);
    }
    if (typeof window !== 'undefined') {
        window.addEventListener('project-loaded', resetProjectInspection);
        window.addEventListener('project-reset', resetProjectInspection);
        window.addEventListener('timeline-reset', resetProjectInspection);
    }
    // Cập nhật circular buffer FPS và vẽ đồ thị sóng biến động thời gian thực (Zero-Allocation)
    function updateFpsGraph(fpsVal, cpuTimeMs, telem, shouldRenderCanvas) {
        const nowMs = telem.nowMs || 0;
        // Reset sạch buffer và số liệu tích lũy khi timeline tua ngược hoặc lặp lại từ đầu (GEMINI.md Rule 5)
        if (lastTelemNowMs > 0 && nowMs < lastTelemNowMs && (lastTelemNowMs - nowMs) > 100) {
            resetHistoricalFps();
            if (window.TimelineEngine && window.TimelineEngine.dirtyStats) {
                for (const k in window.TimelineEngine.dirtyStats) window.TimelineEngine.dirtyStats[k] = 0;
            }
            if (window.__timelineTelemetry && window.__timelineTelemetry.dirtyStats) {
                for (const k in window.__timelineTelemetry.dirtyStats) window.__timelineTelemetry.dirtyStats[k] = 0;
            }
            if (telem) {
                telem.ttHits = 0;
                telem.ttMisses = 0;
                telem.zobristBreakers = 0;
                telem.noneCount = 0;
                telem.fullCount = 0;
                telem.dirtyCount = 0;
            }
        }
        const isPlaying = !!(window.state && window.state.isPlaying);
        // Đẩy giá trị mới vào circular buffer và cập nhật lịch sử khi timeline đang chạy (isPlaying)
        // Khi mới nạp (histFpsCount == 0), nạp đầy buffer bằng fpsVal hiện thời để đường vẽ luôn hiển thị ngay
        if (fpsVal > 0) {
            if (histFpsCount === 0) {
                fpsHistory.fill(fpsVal);
            }
            if (isPlaying) {
                fpsHistory[fpsHistoryIdx] = fpsVal;
                fpsHistoryIdx = (fpsHistoryIdx + 1) % FPS_HISTORY_CAP;
                if (fpsVal < histMinFps) histMinFps = fpsVal;
                if (fpsVal > histMaxFps) histMaxFps = fpsVal;
                histFpsSum += fpsVal;
                histFpsCount++;
            }
        }
        // Tính toán các chỉ số min / avg / max mang tính lịch sử (chỉ reset khi loop hoặc chuyển project)
        const min = histMinFps < Infinity ? histMinFps : fpsVal;
        const max = histMaxFps > 0 ? histMaxFps : fpsVal;
        const avg = histFpsCount > 0 ? (histFpsSum / histFpsCount) : fpsVal;
        const minStr = (histFpsCount > 0 && histMinFps < Infinity) ? histMinFps.toFixed(1) : '--';
        const maxStr = (histFpsCount > 0 && histMaxFps > 0) ? histMaxFps.toFixed(1) : '--';
        const avgStr = (histFpsCount > 0) ? avg.toFixed(1) : '--';
        // Cập nhật DOM textContent in-place
        if (fpsMinVal && fpsMinVal.textContent !== minStr) fpsMinVal.textContent = minStr;
        if (fpsAvgVal && fpsAvgVal.textContent !== avgStr) fpsAvgVal.textContent = avgStr;
        if (fpsMaxVal && fpsMaxVal.textContent !== maxStr) fpsMaxVal.textContent = maxStr;
        // Ngân sách khung hình chuẩn VSync 60 FPS = 16.6ms (Theo GEMINI.md Rule 6)
        const targetMs = 16.6667;
        const budgetPercent = Math.min(100, Math.max(0, (cpuTimeMs / targetMs) * 100));
        const budgetStr = budgetPercent.toFixed(1) + '%';
        const budgetMeterLbl = document.querySelector('.debug-budget-meter-lbl');
        if (budgetMeterLbl && budgetMeterLbl.textContent !== 'BUDGET (16.6ms):') {
            budgetMeterLbl.textContent = 'BUDGET (16.6ms):';
        }
        if (budgetTextVal) {
            budgetTextVal.textContent = `(${budgetStr})`;
            budgetTextVal.style.color = budgetPercent > 90 ? '#ef4444' : (budgetPercent > 70 ? '#f59e0b' : '#828294');
        }
        if (budgetPctVal) budgetPctVal.textContent = budgetStr;
        if (budgetMeterBar) {
            budgetMeterBar.style.width = budgetStr;
            budgetMeterBar.style.backgroundColor = budgetPercent > 90 ? '#ef4444' : (budgetPercent > 70 ? '#f59e0b' : '#10b981');
        }
        // Vẽ đồ thị sóng trên Canvas 2D (chỉ vẽ khi Drawer mở và tab Telemetry active)
        if (!shouldRenderCanvas || !fpsCanvas || !fpsCanvasCtx) return;
        const rect = fpsCanvas.getBoundingClientRect();
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const w = Math.floor(rect.width);
        const h = Math.floor(rect.height);
        if (w <= 0 || h <= 0) return;
        if (fpsCanvas.width !== Math.floor(w * dpr) || fpsCanvas.height !== Math.floor(h * dpr)) {
            fpsCanvas.width = Math.floor(w * dpr);
            fpsCanvas.height = Math.floor(h * dpr);
        }
        const ctx = fpsCanvasCtx;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, w, h);
        // Tỷ lệ trục Y của đồ thị: chỉ bám theo cửa sổ hiển thị hiện thời trong fpsHistory
        // (chỉ số MAX lịch sử được giữ ở Header, đồ thị tự động trở lại mốc bình thường khi đột biến trôi qua)
        let currentWindowMax = fpsVal;
        for (let i = 0; i < FPS_HISTORY_CAP; i++) {
            const v = fpsHistory[i];
            if (v > currentWindowMax) currentWindowMax = v;
        }
        const maxSeen = Math.max(currentWindowMax, 60);
        const maxFpsScale = Math.max(75, Math.ceil((maxSeen + 15) / 30) * 30);
        const getY = (val) => {
            const ratio = Math.max(0, Math.min(1, val / maxFpsScale));
            return (h - 2) - ratio * (h - 6);
        };
        // 1. Đường tham chiếu kỹ thuật (Grid lines nét đứt)
        ctx.lineWidth = 1;
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
        ctx.setLineDash([3, 3]);
        const refLines = [30, 60];
        if (maxFpsScale >= 120 && maxFpsScale < 300) {
            refLines.push(120);
            if (maxFpsScale >= 180) refLines.push(180);
        } else if (maxFpsScale >= 300) {
            refLines.push(120);
            refLines.push(Math.round((maxFpsScale * 0.75) / 30) * 30);
        }
        ctx.fillStyle = 'rgba(255, 255, 255, 0.28)';
        ctx.font = '8px monospace';
        for (const ref of refLines) {
            const yRef = getY(ref);
            ctx.beginPath();
            ctx.moveTo(0, yRef);
            ctx.lineTo(w, yRef);
            ctx.stroke();
            ctx.fillText(String(ref), w - 18, Math.max(8, yRef - 2));
        }
        ctx.setLineDash([]);
        // 2. Vẽ đường sóng biến động FPS
        const stepX = w / (FPS_HISTORY_CAP - 1);
        ctx.beginPath();
        let lastY = 0;
        for (let i = 0; i < FPS_HISTORY_CAP; i++) {
            const dataIndex = (fpsHistoryIdx + i) % FPS_HISTORY_CAP;
            const rawVal = fpsHistory[dataIndex];
            const val = rawVal > 0 ? rawVal : (fpsVal > 0 ? fpsVal : 60);
            const x = i * stepX;
            const y = getY(val);
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
            if (i === FPS_HISTORY_CAP - 1) lastY = y;
        }
        const strokeColor = avg >= 50 ? '#10b981' : (avg >= 30 ? '#f59e0b' : '#ef4444');
        ctx.strokeStyle = strokeColor;
        ctx.lineWidth = 1.5;
        ctx.lineJoin = 'round';
        ctx.stroke();
        // 3. Đổ bóng gradient dưới đường sóng
        ctx.lineTo(w, h);
        ctx.lineTo(0, h);
        ctx.closePath();
        const grad = ctx.createLinearGradient(0, 0, 0, h);
        grad.addColorStop(0, avg >= 50 ? 'rgba(16, 185, 129, 0.25)' : (avg >= 30 ? 'rgba(245, 158, 11, 0.25)' : 'rgba(239, 68, 68, 0.25)'));
        grad.addColorStop(1, 'rgba(0, 0, 0, 0.0)');
        ctx.fillStyle = grad;
        ctx.fill();
        // 4. Chấm sáng thời gian thực ở đầu sóng (Oscilloscope head dot)
        ctx.beginPath();
        ctx.arc(w - 1, lastY, 2, 0, Math.PI * 2);
        ctx.fillStyle = '#ffffff';
        ctx.fill();
    }
    // Cập nhật nội dung trong Drawer khi đang mở
    function updateDrawerPanes(telem) {
        // Tab 1: Telemetry & Cards
        const currentFps = telem.fps || 60;
        if (cardFpsVal) {
            cardFpsVal.textContent = currentFps.toFixed(1);
            cardFpsVal.style.color = currentFps >= 50 ? '#10b981' : (currentFps >= 30 ? '#f59e0b' : '#ef4444');
        }
        if (cardFpsDot) {
            cardFpsDot.style.display = 'none';
        }
        if (cardFrameTimeVal) cardFrameTimeVal.textContent = (telem.frameTimeMs || 16.6).toFixed(1);
        if (cardCpuTimeVal) cardCpuTimeVal.textContent = (telem.cpuTimeMs || 0).toFixed(2);
        if (cardTimeVal) cardTimeVal.textContent = formatTimeMs(telem.nowMs || 0);
        if (cardFrameVal) cardFrameVal.textContent = `F${telem.frameIndex || 0}`;
        const m = telem.renderMode || 'DIRTY';
        const modeLabel = m === 'FULL'
            ? 'FULL'
            : (m === 'ZOBRIST_SKIP' ? 'O(1)' : (m === 'NONE' ? 'NONE' : 'DIRTY'));
        const modeClass = m === 'FULL'
            ? 'full'
            : (m === 'ZOBRIST_SKIP' ? 'breaker' : (m === 'NONE' ? 'none' : 'dirty'));
        if (cardModePill) {
            cardModePill.textContent = modeLabel;
            cardModePill.className = `debug-mode-pill ${modeClass}`;
        }
        if (cardModeDetail) {
            cardModeDetail.textContent = m === 'ZOBRIST_SKIP' ? 'BREAKER' : (m === 'FULL' ? 'PASSES' : 'RUNS');
        }
        if (cardModeStats) {
            cardModeStats.textContent = `(${telem.dirtyN || 0} rects)`;
        }
        if (cardEpochVal) cardEpochVal.textContent = String(telem.styleEpoch || 0);
        const rcPane = (typeof window !== 'undefined' && window.state && window.state.renderConfig) || {};
        const paneBaseW = rcPane.baseW || telem.baseW || 0;
        const paneBaseH = rcPane.baseH || telem.baseH || 0;
        if (cardVerVal) cardVerVal.textContent = `v${telem.fontsVer || 0} / v${telem.mediaVer || 0}`;
        const activePreviewRes2 = (typeof window !== 'undefined' && window.TimelineEngine && window.TimelineEngine.getPreviewResolution) ? window.TimelineEngine.getPreviewResolution() : null;
        const actualRenderW = telem.baseW || (activePreviewRes2 && activePreviewRes2.w) || paneBaseW;
        const actualRenderH = telem.baseH || (activePreviewRes2 && activePreviewRes2.h) || paneBaseH;
        if (cardResVal) cardResVal.textContent = `${paneBaseW}×${paneBaseH}`;
        if (cardCanvasResVal) cardCanvasResVal.textContent = `${actualRenderW}×${actualRenderH}`;
        if (cardScaleVal) cardScaleVal.textContent = `${(telem.scale || 1).toFixed(3)}x`;
        if (cardDprTag) cardDprTag.textContent = `DPR ${(telem.dpr || 1).toFixed(1)}`;
        if (playbackSpeedVal) {
            const spd = (window.clock && window.clock.speed) || 1.0;
            playbackSpeedVal.textContent = Math.abs(spd - 1.0) > 0.05 ? ` · ${spd.toFixed(1)}x` : '';
        }
        if (aspectVal) {
            const bw = paneBaseW | 0;
            const bh = paneBaseH | 0;
            let a = bw, b = bh;
            while (b !== 0) { const t = b; b = a % b; a = t; }
            const g = a || 1;
            const rw = (bw / g) | 0, rh = (bh / g) | 0;
            const orient = bw > bh ? ' Landscape' : (bh > bw ? ' Portrait' : '');
            const ratioStr = (bw > 0 && bh > 0) ? `${rw}:${rh}${orient}` : '--';
            aspectVal.innerHTML = `${ratioStr} <span class="debug-sep">|</span> Fit`;
        }
        if (tabFxCount) tabFxCount.textContent = String(telem.activeFxCount || 0);
        if (tabLayersCount) tabLayersCount.textContent = String(telem.activeLayersCount || 0);
        // Tab 2: Effects Table (chỉ build lại khi tab effects đang active)
        if (activeTabId === 'effects' && effectsTbody) {
            renderEffectsTable(telem);
        }
        // Tab 3: Layers Table (chỉ build lại khi tab layers đang active)
        if (activeTabId === 'layers' && layersTbody) {
            renderLayersTable(telem);
        }
        // Tab 4: Cache & Zobrist
        if (activeTabId === 'cache') {
            const commandMode = telem.commandCacheMode || 'MISS';
            const renderMode = telem.renderMode || 'DIRTY';
            const dirtyRects = Number(telem.dirtyN) || 0;
            const dirtyRectLabel = `${dirtyRects} RECT${dirtyRects === 1 ? '' : 'S'}`;
            if (commandCacheStateVal) {
                commandCacheStateVal.textContent = commandMode === 'HIT'
                    ? 'HIT'
                    : (commandMode === 'SKIP' ? 'ZOBRIST SKIP' : 'MISS');
            }
            if (commandCacheDetailVal) {
                commandCacheDetailVal.textContent = commandMode === 'HIT'
                    ? 'CACHED DISPLAY LIST REUSED'
                    : (commandMode === 'SKIP' ? 'HASH UNCHANGED; BUILD SKIPPED' : 'DISPLAY LIST REBUILT');
            }
            let pixelState = 'DIRTY';
            let pixelDetail = `${dirtyRectLabel} | INCREMENTAL`;
            if (renderMode === 'FULL') {
                pixelState = 'FULL';
                pixelDetail = `${dirtyRectLabel} | ${telem.fullReason || 'SCENE INVALIDATED'}`;
            } else if (renderMode === 'NONE') {
                pixelState = 'NONE';
                pixelDetail = 'SCENE REUSED';
            } else if (renderMode === 'ZOBRIST_SKIP') {
                pixelState = 'REUSE';
                pixelDetail = 'HASH UNCHANGED | NO REDRAW';
            } else if (renderMode === 'TT_HIT') {
                pixelState = 'REUSE';
                pixelDetail = 'COMMAND CACHE HIT | NO REDRAW';
            }
            if (pixelRenderStateVal) pixelRenderStateVal.textContent = pixelState;
            if (pixelRenderDetailVal) pixelRenderDetailVal.textContent = pixelDetail;
            if (zobristHashVal) zobristHashVal.textContent = telem.currentHashHex || '0x0000000000000000';
            const stats = telem.dirtyStats || (window.TimelineEngine && window.TimelineEngine.dirtyStats) || {};
            const ttHits = telem.ttHits !== undefined ? telem.ttHits : (Number(stats.ttHit) || 0);
            const ttMisses = telem.ttMisses !== undefined ? telem.ttMisses : (Number(stats.ttMiss) || 0);
            const ttBreakers = telem.zobristBreakers !== undefined ? telem.zobristBreakers : (Number(stats.zobristBreaker) || 0);
            const cacheLookups = ttHits + ttMisses;
            if (ttHitsVal) ttHitsVal.textContent = String(ttHits);
            if (ttMissesVal) ttMissesVal.textContent = String(ttMisses);
            if (ttBreakerVal) ttBreakerVal.textContent = String(ttBreakers);
            if (statFullVal) statFullVal.textContent = String(telem.fullCount !== undefined ? telem.fullCount : (stats.full || 0));
            if (statDirtyVal) statDirtyVal.textContent = String(telem.dirtyCount !== undefined ? telem.dirtyCount : (stats.dirty || 0));
            if (statNoneVal) statNoneVal.textContent = String(telem.noneCount !== undefined ? telem.noneCount : (stats.none || 0));
            let totalDefined = 0;
            let totalKf = 0;
            let staticCount = 0;
            const tl = (typeof window !== 'undefined' && window.state && window.state.timeline) || [];
            const bridge = window.WasmBridge;
            const posSub = bridge && (bridge.position || bridge.pos);
            if (bridge && bridge.isReady() && posSub && typeof posSub.getTotalDefinedCount === 'function') {
                // Rule 9 GEMINI.md: 100% C++ Core Engine cung cấp số liệu telemetry, JS zero-business!
                totalDefined = posSub.getTotalDefinedCount();
                totalKf = posSub.getTotalKeyframedCount();
                staticCount = posSub.getStaticSlotCount();
            } else {
                for (let i = 0; i < tl.length; i++) {
                    const it = tl[i];
                    if (!it) continue;
                    if (it._definedMask && typeof it._definedMask.popcount === 'function') {
                        totalDefined += it._definedMask.popcount();
                        totalKf += (it._keyframedMask && typeof it._keyframedMask.popcount === 'function') ? it._keyframedMask.popcount() : 0;
                        if (it._isStatic) staticCount++;
                    } else if (it._definedProps !== undefined) {
                        const cBit = (window.TimelineEngine && window.TimelineEngine.countBits64) ? window.TimelineEngine.countBits64 : null;
                        if (cBit) {
                            totalDefined += cBit(it._definedProps);
                            totalKf += cBit(it._keyframedProps);
                        }
                        if (it._isStatic) staticCount++;
                    }
                }
            }
            if (cacheHitRateVal) {
                cacheHitRateVal.textContent = cacheLookups > 0
                    ? `${((ttHits / cacheLookups) * 100).toFixed(1)}%`
                    : '--';
            }
        }
    }
    // Quản lý dirty-check để chỉ khi có thay đổi thật mới cập nhật DOM
    let lastEffectsSig = '';
    let lastLayersSig = '';
    // Hiển thị bảng Effects - CƠ CHẾ KHÔNG RENDER LẠI CONTROL MÀ THAY NỘI DUNG IN-PLACE
    function renderEffectsTable(telem) {
        const count = telem.activeFxCount || 0;
        // Tính signature nhanh: nếu không đổi thật sự thì tuyệt đối không chạm DOM
        let sig = count + ':';
        for (let i = 0; i < count; i++) {
            const fx = telem.activeFxList && telem.activeFxList[i];
            if (fx) {
                sig += `${fx.name}|${fx.z}|${fx.kind}|${fx.start}|${fx.end}|${fx.params};`;
            }
        }
        if (sig === lastEffectsSig) return; // ZERO DOM TOUCH khi không đổi!
        lastEffectsSig = sig;
        if (count === 0) {
            effectsTbody.innerHTML = '<div class="dbg-grid-empty">--</div>';
            return;
        }
        const emptyEl = effectsTbody.querySelector('.dbg-grid-empty');
        if (emptyEl) emptyEl.remove();
        // Chuẩn hóa kích thước hàng đúng bằng count: cắt bớt hoặc tạo thêm
        while (effectsTbody.children.length > count) {
            effectsTbody.removeChild(effectsTbody.lastElementChild);
        }
        while (effectsTbody.children.length < count) {
            const row = document.createElement('div');
            row.className = 'dbg-fx-grid-row dbg-grid-data-row';
            row.innerHTML = `
                <div class="dbg-grid-cell" style="color:#6b7280;"></div>
                <div class="dbg-grid-cell" style="color:#ffffff;"></div>
                <div class="dbg-grid-cell"></div>
                <div class="dbg-grid-cell" style="color:#e0af68;"></div>
                <div class="dbg-grid-cell" style="color:#7aa2f7;"></div>
                <div class="dbg-grid-cell"></div>
            `;
            effectsTbody.appendChild(row);
        }
        // Tái sử dụng control/div row cũ, thay thế nội dung các ô (Zero InnerHTML recreation)
        for (let i = 0; i < count; i++) {
            const fx = telem.activeFxList[i];
            if (!fx) continue;
            const row = effectsTbody.children[i];
            const cIdx = row.children[0];
            const cName = row.children[1];
            const cKind = row.children[2];
            const cZ = row.children[3];
            const cTime = row.children[4];
            const cParams = row.children[5];
            const isValid = Boolean(fx && fx.name);
            const idxStr = isValid ? String(i + 1) : '--';
            if (cIdx.textContent !== idxStr) cIdx.textContent = idxStr;
            const nameStr = (isValid && fx.name) ? fx.name : '--';
            if (cName.textContent !== nameStr) cName.textContent = nameStr;
            const zStr = (isValid && fx.z !== undefined && fx.z !== null) ? `z: ${fx.z}` : '--';
            if (cZ.textContent !== zStr) cZ.textContent = zStr;
            if (isValid && fx.kind) {
                let kindText = 'Ping-Pong';
                if (fx.kind.includes('MIP-Pyramid')) kindText = 'MIP-Pyramid';
                else if (fx.kind.includes('Dual-Pass')) kindText = 'Dual-Pass';
                else if (fx.kind.includes('Ping-Pong')) kindText = 'Ping-Pong';
                else kindText = 'Shader';
                const kindColor = '#7dcfff';
                if (cKind.textContent !== kindText) {
                    cKind.innerHTML = `<span style="color:${kindColor};font-weight:600;">${kindText}</span>`;
                }
            } else {
                if (cKind.textContent !== '--') cKind.textContent = '--';
            }
            const timeStr = (isValid && fx.start !== undefined && fx.end !== undefined)
                ? `${formatTimeMs(fx.start)} - ${formatTimeMs(fx.end)}`
                : '--';
            if (cTime.textContent !== timeStr) cTime.textContent = timeStr;
            const paramsStr = (isValid && (fx.group || fx.params)) ? (fx.group || fx.params) : '--';
            if (cParams.textContent !== paramsStr) {
                cParams.textContent = paramsStr;
                let groupColor = '#9ca3af';
                if (paramsStr === 'transition') groupColor = '#43ad6a';
                else if (paramsStr === 'full-screen') groupColor = '#38bdf8';
                else if (paramsStr === 'full-layer') groupColor = '#10b981';
                else if (paramsStr === 'element') groupColor = '#e0af68';
                cParams.style.color = groupColor;
            }
        }
    }
    // Hiển thị bảng Layers - CƠ CHẾ PURE CSS GRID KHÔNG RENDER LẠI CONTROL MÀ THAY NỘI DUNG IN-PLACE
    function renderLayersTable(telem) {
        const count = telem.activeLayersCount || 0;
        let sig = count + ':';
        for (let i = 0; i < count; i++) {
            const lyr = telem.activeLayersList && telem.activeLayersList[i];
            if (lyr) {
                sig += `${lyr.id}|${lyr.type}|${lyr.z}|${lyr.bounds}|${lyr.state};`;
            }
        }
        if (sig === lastLayersSig) return; // ZERO DOM TOUCH khi không đổi!
        lastLayersSig = sig;
        if (count === 0) {
            layersTbody.innerHTML = '<div class="dbg-grid-empty">--</div>';
            return;
        }
        const emptyEl = layersTbody.querySelector('.dbg-grid-empty');
        if (emptyEl) emptyEl.remove();
        // Chuẩn hóa kích thước hàng đúng bằng count: cắt bớt hoặc tạo thêm
        while (layersTbody.children.length > count) {
            layersTbody.removeChild(layersTbody.lastElementChild);
        }
        while (layersTbody.children.length < count) {
            const row = document.createElement('div');
            row.className = 'dbg-layers-grid-row dbg-grid-data-row';
            row.innerHTML = `
                <div class="dbg-grid-cell" style="color:#6b7280;"></div>
                <div class="dbg-grid-cell" style="color:#ffffff;"></div>
                <div class="dbg-grid-cell"></div>
                <div class="dbg-grid-cell" style="color:#e0af68;"></div>
                <div class="dbg-grid-cell" style="color:#7aa2f7;"></div>
                <div class="dbg-grid-cell"></div>
            `;
            layersTbody.appendChild(row);
        }
        for (let i = 0; i < count; i++) {
            const lyr = telem.activeLayersList[i];
            if (!lyr) continue;
            const row = layersTbody.children[i];
            const cIdx = row.children[0];
            const cId = row.children[1];
            const cType = row.children[2];
            const cZ = row.children[3];
            const cBounds = row.children[4];
            const cState = row.children[5];
            const isValid = Boolean(lyr && lyr.type);
            const idxStr = isValid ? String(i + 1) : '--';
            if (cIdx.textContent !== idxStr) cIdx.textContent = idxStr;
            const idStr = (isValid && lyr.id) ? String(lyr.id) : '--';
            if (cId.textContent !== idStr) cId.textContent = idStr;
            if (isValid) {
                const typeColor = lyr.type === 'video' ? '#7dcfff' : (lyr.type === 'image' ? '#73daca' : (lyr.type === 'path' ? '#f472b6' : '#bb9af7'));
                const typeStr = String(lyr.type);
                if (cType.textContent !== typeStr) {
                    cType.innerHTML = `<span style="color:${typeColor};font-weight:600;">${escapeHtml(typeStr)}</span>`;
                }
            } else {
                if (cType.textContent !== '--') cType.textContent = '--';
            }
            const zStr = (isValid && lyr.z !== undefined && lyr.z !== null) ? `z: ${lyr.z}` : '--';
            if (cZ.textContent !== zStr) cZ.textContent = zStr;
            const boundsStr = (isValid && lyr.bounds && lyr.bounds !== '--') ? String(lyr.bounds) : '--';
            if (cBounds.textContent !== boundsStr) cBounds.textContent = boundsStr;
            if (isValid && lyr.state && lyr.state !== '--') {
                const stateColor = /still|static/i.test(lyr.state) ? '#73daca' : '#ff9e64';
                const stateStr = String(lyr.state);
                if (cState.textContent !== stateStr) {
                    cState.innerHTML = `<span style="color:${stateColor};font-weight:500;">${escapeHtml(stateStr)}</span>`;
                }
            } else {
                if (cState.textContent !== '--') cState.textContent = '--';
            }
        }
    }
    function escapeHtml(str) {
        if (!str) return '';
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');
    }
    // Khởi chạy ngay lập tức nếu elements đã có trong DOM (vì script nằm ở cuối trang)
    try {
        initDOMElements();
        requestAnimationFrame(updateTelemetryDOM);
    } catch (e) { }
    if (document.readyState === 'loading') {
        window.addEventListener('DOMContentLoaded', () => {
            initDOMElements();
            requestAnimationFrame(updateTelemetryDOM);
        });
    }
    window.DebugInspector = {
        init: initDOMElements,
        setHeight: setDrawerHeight,
        toggle: toggleDrawer,
        switchTab,
        reset: resetProjectInspection,
        refresh: () => { if (typeof refreshViewport === 'function') refreshViewport(); },
        update: () => updateTelemetryDOM(performance.now(), true)
    };
})();