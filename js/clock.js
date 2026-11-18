// ============================================================================
// ĐỒNG HỒ ẢO + THANH PHÁT — nguồn thời gian duy nhất của preview/export
//   Phụ thuộc : ui.js, state.js, preview.js (updateFrame), timeline-new.js
//   Cung cấp  : ensureVirtualClock, start/stopVirtualClock, toggleVirtualPlay, initVirtualPlayerUI, getAccurateTimeMs, getProjectDurationMs
// ============================================================================
function ensureVirtualClock() {
    if (!state.virtualClock) {
        state.virtualClock = {
            playing: false,
            baseMs: 0,
            startedAt: 0,
            rate: 1
        };
    }
    return state.virtualClock;
}
function startVirtualClock(fromMs) {
    const vc = ensureVirtualClock();
    vc.baseMs = fromMs || 0;
    vc.startedAt = performance.now();
    vc.rate = parseFloat((typeof UI !== 'undefined' && UI.speedSelect) ? UI.speedSelect.value : 1) || 1;
    vc.playing = true;
    vc.lastRafTime = vc.startedAt;
    vc.hiddenAt = 0;
    state.isPlaying = true;
    state.sceneValid = false;
    // C++ LIVING CORE: Kích hoạt Master Active Clock
    if (window.WasmBridge && window.WasmBridge.clock) {
        window.WasmBridge.clock.setDuration(getProjectDurationMs());
        window.WasmBridge.clock.setRate(vc.rate);
        window.WasmBridge.clock.seek(vc.baseMs);
        window.WasmBridge.clock.play();
    }
    if (typeof UI !== 'undefined' && UI.mediaPlayer && UI.mediaPlayer.src) {
        UI.mediaPlayer.playbackRate = vc.rate;
        const target = (fromMs || 0) / 1000;
        if (Math.abs(UI.mediaPlayer.currentTime - target) > 0.05) {
            try { UI.mediaPlayer.currentTime = target; } catch (e) {}
        }
        UI.mediaPlayer.play().catch(() => {});
    }
    if (window.TimelineEngine && TimelineEngine.setPlaybackRate) {
        TimelineEngine.setPlaybackRate(vc.rate);
    }
    // Kích hoạt đồng bộ media và phát âm thanh ngay lập tức trong user gesture stack
    if (window.TimelineEngine && TimelineEngine.syncMedia) {
        TimelineEngine.syncMedia(vc.baseMs, true);
    }
}
function stopVirtualClock() {
    const vc = ensureVirtualClock();
    if (vc.playing) vc.baseMs = getAccurateTimeMs();
    vc.playing = false;
    state.isPlaying = false;
    // C++ LIVING CORE: Dừng Master Clock
    if (window.WasmBridge && window.WasmBridge.clock) {
        window.WasmBridge.clock.pause();
    }
    if (typeof UI !== 'undefined' && UI.mediaPlayer && !UI.mediaPlayer.paused) {
        try { UI.mediaPlayer.pause(); } catch (e) {}
    }
    // Dừng triệt để 100% âm thanh và video đang phát trong toàn bộ timeline engine
    if (window.TimelineEngine && TimelineEngine.pauseMedia) {
        TimelineEngine.pauseMedia();
    }
}
function toggleVirtualPlay() {
    if (!window.WasmBridge || !window.WasmBridge.isReady()) {
        console.warn('[Clock] WebAssembly Core ch\u01b0a s\u1eb5n s\u00e0ng, vui l\u00f2ng \u0111\u1ee3i gi\u00e2y l\u00e1t...');
        return;
    }
    const vc = ensureVirtualClock();
    if (vc.playing) {
        stopVirtualClock();
        cancelAnimationFrame(state.animationFrameId);
        updateFrame(vc.baseMs);
    } else {
        const dur = getProjectDurationMs();
        if (vc.baseMs >= dur) vc.baseMs = 0;
        startVirtualClock(vc.baseMs);
        vc.lastRafTime = performance.now();
        const loop = (rafTime) => {
            if (!vc.playing) return;
            const curRaf = rafTime || performance.now();
            const deltaMs = Math.max(0, Math.min(100, curRaf - vc.lastRafTime));
            vc.lastRafTime = curRaf;
            if (state.isExporting) {
                state.animationFrameId = requestAnimationFrame(loop);
                return;
            }
            const prevMs = vc.baseMs;
            // C++ LIVING CORE: Master Clock Tick (Tính toán frame logic, sinh DisplayList, loop timeline)
            if (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.clock) {
                window.WasmBridge.clock.tickPreview(deltaMs);
            }
            let timeMs = getAccurateTimeMs();
            if (timeMs >= getProjectDurationMs() || (prevMs > 0 && timeMs < prevMs)) {
                startVirtualClock(0);
                timeMs = 0;
            }
            vc.baseMs = timeMs;
            updateFrame(timeMs);
            state.animationFrameId = requestAnimationFrame(loop);
        };
        state.animationFrameId = requestAnimationFrame(loop);
    }
    updateVirtualPlayBtn();
}
// Nút ▶/⏸: JS chỉ bật/tắt class, biểu tượng do CSS vẽ (.is-playing)
function updateVirtualPlayBtn() {
    const playing = !!(state.virtualClock && state.virtualClock.playing);
    if (typeof UI !== 'undefined' && UI.virtualPlayBtn) {
        UI.virtualPlayBtn.classList.toggle('is-playing', playing);
    }
}
function fmtClock(ms) {
    const t = Math.max(0, Math.floor(ms / 1000));
    return Math.floor(t / 60) + ':' + String(t % 60).padStart(2, '0');
}
// Thanh phát mini [▶] 0:00 / 0:12 [——o——] chạy bằng đồng hồ ảo.
// Markup có sẵn trong index.html (#virtual-player), kiểu dáng ở css/app.css.
// Hàm này chỉ: mở khoá điều khiển, gắn sự kiện (một lần), chạy bộ cập nhật 100ms.
window.addEventListener('wasm-ready', () => {
    if (typeof UI !== 'undefined') {
        if (UI.virtualPlayBtn) UI.virtualPlayBtn.disabled = false;
        if (UI.virtualSeek) UI.virtualSeek.disabled = false;
    }
});
let virtualPlayerReady = false;
function initVirtualPlayerUI() {
    if (typeof UI === 'undefined' || !UI.playerControls) return;
    UI.playerControls.classList.add('is-ready');
    UI.virtualPlayBtn.disabled = false;
    UI.virtualSeek.disabled = false;
    updateVirtualPlayBtn();
    if (virtualPlayerReady) return;
    virtualPlayerReady = true;
    const btn = UI.virtualPlayBtn, time = UI.virtualTime, seek = UI.virtualSeek;
    btn.addEventListener('click', toggleVirtualPlay);
    document.addEventListener('visibilitychange', () => {
        const vc = ensureVirtualClock();
        if (document.visibilityState !== 'visible') {
            if (vc.playing) vc.hiddenAt = performance.now();
            return;
        }
        if (!vc.playing) {
            vc.hiddenAt = 0;
            return;
        }
        const now = performance.now();
        const dur = getProjectDurationMs();
        const mediaTime = window.TimelineEngine && TimelineEngine.getPlaybackMediaTimeMs
            ? TimelineEngine.getPlaybackMediaTimeMs()
            : null;
        let targetMs = vc.baseMs;
        if (mediaTime !== null && Number.isFinite(mediaTime) && mediaTime > 0) {
            targetMs = mediaTime;
        } else if (vc.hiddenAt > 0) {
            targetMs += (now - vc.hiddenAt) * (vc.rate || 1);
        }
        if (dur > 0 && targetMs >= dur) {
            targetMs = targetMs % dur;
        }
        vc.baseMs = targetMs;
        vc.startedAt = now;
        vc.lastRafTime = now;
        vc.hiddenAt = 0;
        if (window.WasmBridge && window.WasmBridge.clock) {
            window.WasmBridge.clock.setDuration(dur);
            window.WasmBridge.clock.setRate(vc.rate);
            window.WasmBridge.clock.seek(targetMs);
            window.WasmBridge.clock.play();
        }
        updateFrame(targetMs);
    });
    let dragging = false;
    let wasPlayingBeforeDrag = false;
    seek.addEventListener('pointerdown', () => {
        dragging = true;
        const vc = ensureVirtualClock();
        wasPlayingBeforeDrag = vc.playing;
        if (wasPlayingBeforeDrag) stopVirtualClock();
    });
    seek.addEventListener('pointerup', () => {
        if (!dragging) return;
        dragging = false;
        const targetMs = (seek.value / 1000) * getProjectDurationMs();
        if (wasPlayingBeforeDrag) {
            startVirtualClock(targetMs);
        }
    });
    seek.addEventListener('input', () => {
        const vc = ensureVirtualClock();
        const targetMs = (seek.value / 1000) * getProjectDurationMs();
        vc.baseMs = targetMs;
        seek.style.setProperty('--seek-pct', (seek.value / 10) + '%');
        if (window.CanvasCompositor && window.CanvasCompositor.invalidateScene) {
            window.CanvasCompositor.invalidateScene('SEEK');
        }
        if (window.WasmBridge && window.WasmBridge.clock) {
            window.WasmBridge.clock.seek(targetMs);
        }
        updateFrame(targetMs);
        time.textContent = fmtClock(targetMs) + ' / ' + fmtClock(getProjectDurationMs());
    });
    seek.addEventListener('change', () => {
        if (dragging) return;
        seek.style.setProperty('--seek-pct', (seek.value / 10) + '%');
        const targetMs = (seek.value / 1000) * getProjectDurationMs();
        if (wasPlayingBeforeDrag) {
            startVirtualClock(targetMs);
        }
    });
    // Xử lý thay đổi tốc độ preview mượt mà — triệt tiêu 100% hiện tượng giật mốc thời gian (time jump)
    if (UI.speedSelect) {
        const onSpeedChange = () => {
            const vc = ensureVirtualClock();
            const newRate = parseFloat(UI.speedSelect.value) || 1;
            if (vc.playing) {
                // Chốt mốc thời gian chính xác tại thời điểm đổi tốc độ vào baseMs,
                // và đặt lại startedAt = performance.now()
                const nowMs = getAccurateTimeMs();
                vc.baseMs = nowMs;
                vc.startedAt = performance.now();
                vc.rate = newRate;
            } else {
                vc.rate = newRate;
            }
            if (window.TimelineEngine && TimelineEngine.setPlaybackRate) {
                TimelineEngine.setPlaybackRate(newRate);
            }
            updateFrame(vc.playing ? getAccurateTimeMs() : vc.baseMs);
        };
        UI.speedSelect.addEventListener('change', onSpeedChange);
        UI.speedSelect.addEventListener('input', onSpeedChange);
    }
    // Phím tắt Spacebar bật/tắt phát/dừng toàn hệ thống
    window.addEventListener('keydown', (e) => {
        const tag = (e.target && e.target.tagName) || '';
        const isTyping = tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'SELECT' || (e.target && e.target.isContentEditable);
        if (isTyping) return;
        if (e.code === 'Space' || e.key === ' ') {
            e.preventDefault();
            toggleVirtualPlay();
        }
    });
    state.virtualUiTimer = setInterval(() => {
        const dur = getProjectDurationMs();
        const cur = Math.min(getAccurateTimeMs(), dur);
        time.textContent = fmtClock(cur) + ' / ' + fmtClock(dur);
        if (!dragging && dur > 0) {
            seek.value = Math.round(cur / dur * 1000);
            seek.style.setProperty('--seek-pct', (cur / dur * 100) + '%');
        }
        updateVirtualPlayBtn();
    }, 100);
}
// Tổng thời lượng dự án (ms): mốc end lớn nhất trong timeline (hoặc config.durationMs nếu lớn hơn)
function getProjectDurationMs() {
    let d = (window.TimelineEngine && TimelineEngine.getDurationMs()) || 0;
    if (state.config && state.config.durationMs > d) d = state.config.durationMs;
    return d > 0 ? d : 30000;
}
// 3. Đồng hồ dự án: luôn là đồng hồ C++ Wasm (Zero-Copy từ Shared Memory 0ns)
function getAccurateTimeMs() {
    if (window.WasmBridge && typeof window.WasmBridge.isReady === 'function' && window.WasmBridge.isReady() && window.WasmBridge.clock) {
        try {
            return window.WasmBridge.clock.getTime();
        } catch (_) {}
    }
    return (window.state && window.state.currentTime) || 0.0;
}
// Mở khoá điều khiển player ngay khi nạp trang
if (typeof document !== 'undefined') {
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initVirtualPlayerUI);
    } else {
        initVirtualPlayerUI();
    }
}
// User-gesture unlocker cho âm thanh trình duyệt
function unlockAudioEngineGesture() {
    if (window.TimelineEngine && TimelineEngine.unlockAudio) {
        TimelineEngine.unlockAudio();
    }
}
window.addEventListener('click', unlockAudioEngineGesture, { passive: true });
window.addEventListener('keydown', unlockAudioEngineGesture, { passive: true });
window.addEventListener('pointerdown', unlockAudioEngineGesture, { passive: true });
