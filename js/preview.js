// ============================================================================
// PREVIEW — vẽ 1 khung hình tại thời điểm t
//   Phụ thuộc : ui.js, state.js, clock.js, timeline-new.js
//   Cung cấp  : updateFrame
// ============================================================================
let _lastTimeDisplaySlot = -1;
let _lastProgressStr = '';
// 4. Cập nhật 1 khung hình preview tại thời điểm currentTimeMs
function updateFrame(forceTimeMs = null) {
    const currentTimeMs = forceTimeMs !== null ? forceTimeMs : getAccurateTimeMs();
    // Zero-Reflow DOM: dùng textContent thay vì innerText, cập nhật mượt mà theo native refresh rate
    const curMsFloor = Math.floor(currentTimeMs);
    if (forceTimeMs !== null || curMsFloor !== _lastTimeDisplaySlot) {
        _lastTimeDisplaySlot = curMsFloor;
        UI.timeDisplay.textContent = curMsFloor + " ms";
    }
    const durationMs = getProjectDurationMs();
    if (durationMs > 0) {
        const progress = Math.max(0, Math.min(100, (currentTimeMs / durationMs) * 100));
        const progStr = progress.toFixed(2);
        if (progStr !== _lastProgressStr) {
            _lastProgressStr = progStr;
            UI.waveformProgress.style.setProperty('--progress', progStr);
        }
    }
    if (window.renderTimelineFrame) {
        try {
            renderTimelineFrame(currentTimeMs, UI.engineCanvas || UI.viewport);
        } catch (err) {
            console.warn('[Preview] renderTimelineFrame error:', err);
        }
    }
    if (window.SelectionTool && typeof window.SelectionTool.onFrameUpdate === 'function') {
        window.SelectionTool.onFrameUpdate();
    }
}
