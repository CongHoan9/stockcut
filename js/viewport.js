// ============================================================================
// VIEWPORT — kích thước khung render theo tỉ lệ + Fullscreen + Hand Mode & Zoom
//   Phụ thuộc : ui.js, state.js, preview.js, clock.js
//   Cung cấp  : initViewportSize, refreshViewport, isViewportFullscreen, toggleViewportFullscreen
// ============================================================================
// Khởi tạo các trạng thái Viewport Tool, Zoom & Pan
if (!state.viewportTool) state.viewportTool = 'pointer';
if (!state.viewportZoom) state.viewportZoom = 1.0;
if (!state.viewportPan) state.viewportPan = { x: 0, y: 0 };
let isPanning = false;
let panStart = { x: 0, y: 0, initPanX: 0, initPanY: 0 };
let tempSpaceHand = false;
let prevToolBeforeSpace = 'pointer';
function applyViewportTransform() {
    if (!UI.viewport) return;
    const pan = state.viewportPan || { x: 0, y: 0 };
    UI.viewport.style.transform = `translate(${pan.x}px, ${pan.y}px)`;
}
function resetViewportPan(shouldSync = true) {
    state.viewportPan = { x: 0, y: 0 };
    applyViewportTransform();
    if (shouldSync) {
        syncViewportToConfig();
    }
}
function syncViewportToConfig() {
    if (!state.config) state.config = {};
    if (state.config.lyricSize !== undefined) delete state.config.lyricSize;
    const zoomPct = (state.viewportZoom === 'fit') ? 'fit' : Math.round((state.viewportZoom || 1.0) * 100);
    const pan = state.viewportPan || { x: 0, y: 0 };
    state.config.viewport = {
        x: Math.round(pan.x),
        y: Math.round(pan.y),
        zoom: zoomPct
    };
    // 1. Đồng bộ tức thì xuống C++ Core Engine (Stockfish ConfigOptionsMap, dirty = null)
    if (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.config) {
        if (typeof window.WasmBridge.config.setDouble === 'function') {
            window.WasmBridge.config.setDouble('viewport.x', state.config.viewport.x);
            window.WasmBridge.config.setDouble('viewport.y', state.config.viewport.y);
            window.WasmBridge.config.setDouble('viewport.zoom', typeof zoomPct === 'number' ? zoomPct : 100);
        }
    }
    // 2. Đồng bộ vào hệ thống Tab JSON chuyên biệt (cập nhật fullProjectData.config, tabTextBuffers['config'], state.ram.projectJson)
    if (typeof window.updateProjectTabData === 'function') {
        window.updateProjectTabData('config', state.config, {
            syncEngine: false,
            focusEditor: false
        });
    } else {
        if (state.ram && state.ram.projectJson) {
            try {
                const data = JSON.parse(state.ram.projectJson);
                delete data.config.lyricSize;
                data.config = state.config;
                state.ram.projectJson = JSON.stringify(data, null, " ");
            } catch (_) {}
        }
        if (typeof currentJsonTab !== 'undefined' && currentJsonTab === 'config' && window.UI && UI.jsonEditor) {
            UI.jsonEditor.value = JSON.stringify(state.config, null, " ");
        }
    }
    if (typeof persistSessionProject === 'function' && state.sessionProjectName) {
        persistSessionProject(state.sessionProjectName, state.ram.projectJson, state.ram.lyricsTxt);
    }
}
function restoreViewportFromConfig() {
    let vp = null;
    if (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.config) {
        const vx = window.WasmBridge.config.getDouble('viewport.x', NaN);
        const vy = window.WasmBridge.config.getDouble('viewport.y', NaN);
        const vz = window.WasmBridge.config.getDouble('viewport.zoom', NaN);
        if (!isNaN(vx) && !isNaN(vy) && !isNaN(vz)) {
            vp = { x: vx, y: vy, zoom: vz };
        }
    }
    if (!vp && state.config && state.config.viewport) {
        vp = state.config.viewport;
    }
    if (!vp) {
        initViewportSize();
        return;
    }
    if (typeof vp.x === 'number' && typeof vp.y === 'number') {
        state.viewportPan = { x: vp.x, y: vp.y };
    }
    if (vp.zoom !== undefined) {
        setViewportZoom(vp.zoom, false);
        applyViewportTransform();
        return;
    }
    initViewportSize();
    applyViewportTransform();
}
// 2. Hàm thiết lập kích thước chuẩn cho khung render (theo tỉ lệ trong config)
function initViewportSize() {
    // Chỉ tính toán lại kích thước nếu đã tải dự án
    if (!state.renderConfig) return;
    const rc = state.renderConfig;
    let maxWidth, maxHeight;
    if (isViewportFullscreen()) {
        // Chế độ toàn màn hình: lấp đầy trọn màn hình vật lý
        maxWidth = window.innerWidth;
        maxHeight = window.innerHeight;
    } else {
        const stage = UI.fullscreenStage;
        if (stage && stage.clientHeight > 0 && stage.clientWidth > 0) {
            maxWidth = stage.clientWidth;
            maxHeight = stage.clientHeight;
        } else {
            const workspace = stage ? stage.parentElement : null;
            const playerHeight = UI.playerWrapper ? UI.playerWrapper.offsetHeight : 150;
            maxWidth = workspace ? workspace.clientWidth : window.innerWidth;
            maxHeight = workspace ? Math.max(50, workspace.clientHeight - playerHeight - 50) : 400;
        }
    }
    const fitScale = Math.min(maxWidth / rc.baseW, maxHeight / rc.baseH);
    const zoomMul = (state.viewportZoom === 'fit' || !state.viewportZoom) ? 1.0 : state.viewportZoom;
    const scale = fitScale * zoomMul;
    state.currentScale = scale;
    // JS chỉ truyền SỐ; CSS (#render-viewport) quyết định dùng nó cho width/height
    UI.viewport.style.setProperty('--vp-w', (rc.baseW * scale) + 'px');
    UI.viewport.style.setProperty('--vp-h', (rc.baseH * scale) + 'px');
    applyViewportTransform();
    if (window.SelectionTool && typeof window.SelectionTool.syncViewBox === 'function') {
        window.SelectionTool.syncViewBox();
    }
}
// Cập nhật lại khung render nếu user thay đổi kích thước cửa sổ trình duyệt
// Tính lại khung + vẽ lại 1 frame. Khi đang tạm dừng không có vòng rAF nào gọi updateFrame,
// nên nếu không vẽ lại thì scale của nội dung (k trong layoutStage) bị giữ giá trị cũ.
function refreshViewport() {
    initViewportSize();
    if (!state.isExporting && !state.isPlaying && window.WasmBridge && window.WasmBridge.isReady()) {
        updateFrame(getAccurateTimeMs());
    }
}
window.addEventListener('resize', refreshViewport);
window.addEventListener('orientationchange', () => {
    setTimeout(refreshViewport, 150);
});
// ============================================================
// [FEAT] Toàn màn hình (Fullscreen) cho khung render-viewport
// Fullscreen thực bằng Fullscreen API, nhắm vào #fullscreen-stage (khung bọc
// ngoài #render-viewport) thay vì chính #render-viewport, để trình duyệt không
// ép width/height 100% (làm méo tỷ lệ khung hình) lên phần tử có kích thước
// pixel do JS tính toán. #fullscreen-stage khi fullscreen sẽ nền đen + căn giữa
// #render-viewport theo đúng kích thước/tỷ lệ đã tính (letterbox tự nhiên).
// ============================================================
function isViewportFullscreen() {
    return document.fullscreenElement === UI.fullscreenStage ||
        document.webkitFullscreenElement === UI.fullscreenStage;
}
async function toggleViewportFullscreen() {
    try {
        if (!isViewportFullscreen()) {
            const aiDrawer = document.getElementById('ai-lyrics-drawer');
            const exportDrawer = document.getElementById('export-drawer');
            const mediaDrawer = document.getElementById('media-drawer');
            if (aiDrawer) aiDrawer.classList.remove('is-open');
            if (exportDrawer) exportDrawer.classList.remove('is-open');
            if (mediaDrawer) mediaDrawer.classList.remove('is-open');
            const aiBtn = document.getElementById('ai-lyrics-btn');
            const mediaBtn = document.getElementById('media-btn');
            if (aiBtn) aiBtn.classList.remove('is-active');
            if (UI.exportBtn) UI.exportBtn.classList.remove('is-active');
            if (mediaBtn) mediaBtn.classList.remove('is-active');
            if (UI.fullscreenStage.requestFullscreen) {
                await UI.fullscreenStage.requestFullscreen();
            } else if (UI.fullscreenStage.webkitRequestFullscreen) {
                UI.fullscreenStage.webkitRequestFullscreen();
            }
        } else {
            if (document.exitFullscreen) {
                await document.exitFullscreen();
            } else if (document.webkitExitFullscreen) {
                document.webkitExitFullscreen();
            }
        }
    } catch (e) {
        console.warn("Không thể chuyển chế độ toàn màn hình:", e);
    }
}
function updateFullscreenBtnLabel() {
    if (!UI.fullscreenBtn) return;
    // Nhãn nằm trong HTML (2 <span>), CSS chọn span nào hiện theo class .is-fullscreen
    UI.fullscreenBtn.classList.toggle('is-fullscreen', isViewportFullscreen());
}
['fullscreenchange', 'webkitfullscreenchange'].forEach(evt => {
    document.addEventListener(evt, () => {
        updateFullscreenBtnLabel();
        // Kích thước khung tham chiếu thay đổi hoàn toàn khi vào/thoát fullscreen -> tính lại + vẽ lại
        refreshViewport();
        // Trình duyệt có thể bắn event trước khi layout xong (innerWidth/innerHeight còn giá trị cũ)
        // -> tính lại thêm ở 2 frame kế tiếp để chắc chắn khớp kích thước thật.
        requestAnimationFrame(() => {
            refreshViewport();
            requestAnimationFrame(refreshViewport);
        });
    });
});
if (UI.fullscreenBtn) {
    UI.fullscreenBtn.addEventListener('click', toggleViewportFullscreen);
}
// Double-click lên khung render: nếu đang ở Hand mode thì reset vị trí (pan: 0, 0); nếu ở Pointer mode thì bật/tắt Fullscreen
if (UI.viewport) {
    UI.viewport.addEventListener('dblclick', (e) => {
        if (state.viewportTool === 'hand') {
            e.stopPropagation();
            resetViewportPan();
            return;
        }
        toggleViewportFullscreen();
    });
}
// Phím tắt bật/tắt Fullscreen — bỏ qua khi đang gõ trong ô nhập liệu (json editor, input...).
window.addEventListener('keydown', (e) => {
    const tag = (e.target && e.target.tagName) || '';
    const isTyping = tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'SELECT' || (e.target && e.target.isContentEditable);
    if (isTyping) return;
    if (e.key === 'F11') {
        e.preventDefault();
        toggleViewportFullscreen();
    } else if (e.key.toLowerCase() === 'f' && !e.ctrlKey && !e.metaKey && !e.altKey) {
        toggleViewportFullscreen();
    }
});
// ============================================================================
// [FEAT] ĐIỀU KHIỂN CÔNG CỤ VIEWPORT: POINTER, HAND (DRAG & PAN), ZOOM DROPDOWN
// ============================================================================
function setViewportTool(tool) {
    state.viewportTool = tool;
    const pointerBtn = document.getElementById('tool-pointer-btn');
    const handBtn = document.getElementById('tool-hand-btn');
    if (pointerBtn) pointerBtn.classList.toggle('active', tool === 'pointer');
    if (handBtn) handBtn.classList.toggle('active', tool === 'hand');
    if (UI.fullscreenStage) {
        UI.fullscreenStage.classList.toggle('mode-hand', tool === 'hand');
        if (tool !== 'hand') {
            UI.fullscreenStage.classList.remove('is-grabbing');
            document.body.classList.remove('viewport-is-panning');
            isPanning = false;
        }
    }
    if (UI.viewport) {
        UI.viewport.classList.toggle('mode-hand', tool === 'hand');
    }
    if (tool !== 'pointer' && window.SelectionTool && typeof window.SelectionTool.clearSelection === 'function') {
        window.SelectionTool.clearSelection();
    }
}
function setViewportZoom(zoomVal, shouldSync = true) {
    if (zoomVal === 'fit') {
        state.viewportZoom = 1.0;
        resetViewportPan(false);
    } else {
        const parsed = parseFloat(zoomVal);
        state.viewportZoom = (!isNaN(parsed) && parsed > 0) ? (parsed / 100) : 1.0;
    }
    const valEl = document.getElementById('viewport-zoom-val');
    if (valEl) {
        valEl.textContent = (zoomVal === 'fit') ? 'Fit' : (Math.round(state.viewportZoom * 100) + '%');
    }
    document.querySelectorAll('.zoom-opt-item').forEach(btn => {
        const bz = btn.getAttribute('data-zoom');
        btn.classList.toggle('active', (zoomVal === 'fit' && bz === 'fit') || (parseFloat(bz) === Math.round(state.viewportZoom * 100)));
    });
    initViewportSize();
    if (!state.isExporting && !state.isPlaying && window.WasmBridge && window.WasmBridge.isReady()) {
        updateFrame(getAccurateTimeMs());
    }
    if (shouldSync) {
        syncViewportToConfig();
    }
}
function initViewportControls() {
    const pointerBtn = document.getElementById('tool-pointer-btn');
    const handBtn = document.getElementById('tool-hand-btn');
    const zoomBtn = document.getElementById('viewport-zoom-btn');
    const zoomMenu = document.getElementById('viewport-zoom-menu');
    if (pointerBtn) {
        pointerBtn.addEventListener('click', () => setViewportTool('pointer'));
    }
    if (handBtn) {
        handBtn.addEventListener('click', () => setViewportTool('hand'));
    }
    if (zoomBtn && zoomMenu) {
        zoomBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const isShown = zoomMenu.style.display !== 'none';
            zoomMenu.style.display = isShown ? 'none' : 'flex';
            zoomBtn.classList.toggle('active', !isShown);
        });
        document.querySelectorAll('.zoom-opt-item').forEach(item => {
            item.addEventListener('click', (e) => {
                e.stopPropagation();
                const zoomVal = item.getAttribute('data-zoom');
                setViewportZoom(zoomVal);
                zoomMenu.style.display = 'none';
                zoomBtn.classList.remove('active');
            });
        });
        document.addEventListener('click', (e) => {
            if (!zoomMenu.contains(e.target) && !zoomBtn.contains(e.target)) {
                zoomMenu.style.display = 'none';
                zoomBtn.classList.remove('active');
            }
        });
    }
    // Kéo thả khung nhìn bằng Hand tool (drag & grabbing cursor)
    if (UI.fullscreenStage) {
        UI.fullscreenStage.addEventListener('mousedown', (e) => {
            if (state.viewportTool === 'hand' || tempSpaceHand) {
                isPanning = true;
                panStart = {
                    x: e.clientX,
                    y: e.clientY,
                    initPanX: (state.viewportPan && state.viewportPan.x) || 0,
                    initPanY: (state.viewportPan && state.viewportPan.y) || 0
                };
                UI.fullscreenStage.classList.add('is-grabbing');
                document.body.classList.add('viewport-is-panning');
                e.preventDefault();
            }
        });
        // Reset pan khi double click trong chế độ hand
        UI.fullscreenStage.addEventListener('dblclick', (e) => {
            if (state.viewportTool === 'hand') {
                e.stopPropagation();
                resetViewportPan();
            }
        });
        // Zoom bằng cuộn chuột khi giữ phím Ctrl/Alt
        UI.fullscreenStage.addEventListener('wheel', (e) => {
            if (e.ctrlKey || e.altKey || state.viewportTool === 'hand') {
                e.preventDefault();
                const delta = e.deltaY < 0 ? 0.1 : -0.1;
                let newZoom = Math.max(0.25, Math.min(4.0, (state.viewportZoom || 1.0) + delta));
                setViewportZoom(Math.round(newZoom * 100));
            }
        }, { passive: false });
    }
    window.addEventListener('mousemove', (e) => {
        if (isPanning) {
            const dx = e.clientX - panStart.x;
            const dy = e.clientY - panStart.y;
            state.viewportPan = {
                x: panStart.initPanX + dx,
                y: panStart.initPanY + dy
            };
            applyViewportTransform();
        }
    });
    window.addEventListener('mouseup', () => {
        if (isPanning) {
            isPanning = false;
            if (UI.fullscreenStage) {
                UI.fullscreenStage.classList.remove('is-grabbing');
            }
            document.body.classList.remove('viewport-is-panning');
            syncViewportToConfig();
        }
    });
    // Phím tắt V (Pointer), H (Hand), và giữ Spacebar để tạm thời kéo khung hình
    window.addEventListener('keydown', (e) => {
        const tag = (e.target && e.target.tagName) || '';
        const isTyping = tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'SELECT' || (e.target && e.target.isContentEditable);
        if (isTyping) return;
        if (e.code === 'Space' && !e.repeat && !tempSpaceHand) {
            if (state.viewportTool !== 'hand') {
                prevToolBeforeSpace = state.viewportTool;
                tempSpaceHand = true;
                setViewportTool('hand');
            }
        } else if (e.key.toLowerCase() === 'v' && !e.ctrlKey && !e.metaKey) {
            setViewportTool('pointer');
        } else if (e.key.toLowerCase() === 'h' && !e.ctrlKey && !e.metaKey) {
            setViewportTool('hand');
        }
    });
    window.addEventListener('keyup', (e) => {
        if (e.code === 'Space' && tempSpaceHand) {
            tempSpaceHand = false;
            setViewportTool(prevToolBeforeSpace);
        }
    });
}
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initViewportControls);
} else {
    initViewportControls();
}
if (typeof window !== 'undefined') {
    window.setViewportZoom = setViewportZoom;
    window.setViewportTool = setViewportTool;
    window.applyViewportTransform = applyViewportTransform;
    window.resetViewportPan = resetViewportPan;
    window.initViewportSize = initViewportSize;
    window.restoreViewportFromConfig = restoreViewportFromConfig;
    window.syncViewportToConfig = syncViewportToConfig;
}
