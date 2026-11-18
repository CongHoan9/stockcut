// ============================================================================
// WAVEFORM — vẽ sóng âm, zoom/cuộn, bấm để tua
//   Phụ thuộc : ui.js, state.js, clock.js, preview.js, timeline-new.js
//   Cung cấp  : refreshTimelineWaveform, renderWaveform, resetWaveform
// ============================================================================
// --- Waveform Rendering Engine (Olive / Premiere NLE Background Worker Peak Generator) ---
let cachedAudioBuffer = null;
const _wavePeakCache = new Map();
let _waveWorker = null;
let _waveJobId = 0;
function _getWaveformWorker() {
    if (_waveWorker)
    {
        return _waveWorker;
    }
    if (typeof Worker === 'undefined' || typeof Blob === 'undefined' || typeof URL === 'undefined') return null;
    try {
        const workerCode = `
            let storedSamples = null;
            self.onmessage = function(e) {
                const msg = e.data;
                if (msg.type === 'INIT_SAMPLES') {
                    storedSamples = msg.samples;
                    return;
                }
                if (msg.type === 'COMPUTE_PEAKS' && storedSamples) {
                    const totalWidth = msg.totalWidth;
                    const len = storedSamples.length;
                    const blockSize = Math.max(1, Math.floor(len / totalWidth));
                    const peaks = new Float32Array(totalWidth);
                    // Strided sampling for massive buffers to guarantee <2ms worker turnaround
                    const step = blockSize > 256 ? Math.floor(blockSize / 128) : 1;
                    for (let i = 0; i < totalWidth; i++) {
                        let sum = 0;
                        let count = 0;
                        const base = i * blockSize;
                        for (let j = 0; j < blockSize; j += step) {
                            sum += Math.abs(storedSamples[base + j]);
                            count++;
                        }
                        peaks[i] = count > 0 ? (sum / count) : 0;
                    }
                    self.postMessage({ type: 'PEAKS_READY', jobId: msg.jobId, totalWidth: totalWidth, peaks: peaks }, [peaks.buffer]);
                }
            };
        `;
        const blob = new Blob([workerCode], { type: 'application/javascript' });
        _waveWorker = new Worker(URL.createObjectURL(blob));
        _waveWorker.onmessage = function (e) {
            const data = e.data;
            if (data && data.type === 'PEAKS_READY' && data.peaks) {
                _wavePeakCache.set(data.totalWidth, data.peaks);
                if (data.jobId === _waveJobId && cachedAudioBuffer) {
                    _drawPeaksToCanvas(data.peaks, data.totalWidth);
                }
            }
        };
    } catch (err) {
        _waveWorker = null;
    }
    return _waveWorker;
}
function _drawPeaksToCanvas(peaks, totalWidth) {
    const canvas = UI.waveformCanvas;
    if (!canvas) return;
    if (canvas.width !== totalWidth) canvas.width = totalWidth;
    const h = UI.waveformContainer.clientHeight || 70;
    if (canvas.height !== h) canvas.height = h;
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    const centerY = canvas.height / 2;
    ctx.fillStyle = "#666";
    for (let i = 0; i < totalWidth; i++) {
        const barHeight = Math.max(1, peaks[i] * canvas.height * 2.5);
        ctx.fillRect(i, centerY - barHeight / 2, 1, barHeight);
    }
}
// Xoá sóng âm + đưa con trỏ tiến độ về 0 (gọi khi nạp dự án mới)
function resetWaveform() {
    cachedAudioBuffer = null;
    _wavePeakCache.clear();
    UI.waveformCanvas.getContext("2d").clearRect(0, 0, UI.waveformCanvas.width, UI.waveformCanvas.height);
    UI.waveformProgress.style.setProperty('--progress', 0);
}
// [TIMELINE] Vẽ sóng âm từ các item audio/video trong timeline (khi không có audio kiểu objects)
let tlWaveSig = '';
async function refreshTimelineWaveform() {
    if (!window.TimelineEngine) return;
    const dur = getProjectDurationMs();
    const sig = JSON.stringify([
        dur,
        TimelineEngine.audioSources().map(it => [
            it.src,
            it.start,
            it.end,
            it.volume !== undefined ? it.volume : (it.volunm !== undefined ? it.volunm : it.vol),
            Array.isArray(it.keyframes || it.keyframe) ? (it.keyframes || it.keyframe).map(k => [k.offset !== undefined ? k.offset : k.time, (k.css && k.css['--volume']) !== undefined ? k.css['--volume'] : (k.volume !== undefined ? k.volume : (k.volunm !== undefined ? k.volunm : k.vol))]) : []
        ])
    ]);
    if (sig === tlWaveSig && cachedAudioBuffer) return;
    tlWaveSig = sig;
    try {
        const buf = await TimelineEngine.buildWaveformBuffer(dur);
        if (sig !== tlWaveSig) return;   // đã có yêu cầu mới hơn
        cachedAudioBuffer = buf;
        _wavePeakCache.clear();
        if (buf) {
            const worker = _getWaveformWorker();
            if (worker) {
                const rawCopy = new Float32Array(buf.getChannelData(0));
                worker.postMessage({ type: 'INIT_SAMPLES', samples: rawCopy }, [rawCopy.buffer]);
            }
            currentZoomLevel = 1;
            renderWaveform(currentZoomLevel);
        } else {
            UI.waveformCanvas.getContext("2d").clearRect(0, 0, UI.waveformCanvas.width, UI.waveformCanvas.height);
        }
    } catch (e) {
        console.error("Lỗi vẽ sóng âm timeline:", e);
    }
}
let currentZoomLevel = 1;
function renderWaveform(zoomLevel) {
    if (!cachedAudioBuffer) return;
    const containerWidth = UI.waveformContainer.clientWidth || 800;
    const totalWidth = Math.max(1, Math.floor(containerWidth * zoomLevel));
    UI.waveformInner.style.setProperty('--wave-w', totalWidth + 'px');
    const cachedPeaks = _wavePeakCache.get(totalWidth);
    if (cachedPeaks) {
        _drawPeaksToCanvas(cachedPeaks, totalWidth);
        return;
    }
    const worker = _getWaveformWorker();
    if (worker) {
        _waveJobId++;
        worker.postMessage({ type: 'COMPUTE_PEAKS', jobId: _waveJobId, totalWidth: totalWidth });
        return;
    }
    // Fallback đồng bộ nếu môi trường không hỗ trợ Web Worker
    const rawData = cachedAudioBuffer.getChannelData(0);
    const blockSize = Math.max(1, Math.floor(rawData.length / totalWidth));
    const step = blockSize > 256 ? Math.floor(blockSize / 128) : 1;
    const peaks = new Float32Array(totalWidth);
    for (let i = 0; i < totalWidth; i++) {
        let sum = 0, count = 0;
        const base = i * blockSize;
        for (let j = 0; j < blockSize; j += step) {
            sum += Math.abs(rawData[base + j]);
            count++;
        }
        peaks[i] = count > 0 ? (sum / count) : 0;
    }
    _wavePeakCache.set(totalWidth, peaks);
    _drawPeaksToCanvas(peaks, totalWidth);
}
// Hỗ trợ cuộn ngang và Zoom bằng chuột giữa (Mouse Wheel)
UI.waveformContainer.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (e.ctrlKey) {
        // Zoom bằng Ctrl + Lăn chuột (Lấy chuột làm tâm zoom)
        const zoomDelta = e.deltaY < 0 ? 0.5 : -0.5;
        const oldZoom = currentZoomLevel;
        currentZoomLevel = Math.max(1, Math.min(20, currentZoomLevel + zoomDelta));
        if (oldZoom !== currentZoomLevel) {
            const rect = UI.waveformContainer.getBoundingClientRect();
            const mouseX = e.clientX - rect.left; // Tọa độ chuột trong khung nhìn
            const scrollLeft = UI.waveformContainer.scrollLeft;
            const oldTotalWidth = UI.waveformInner.offsetWidth;
            // Tính tỉ lệ phần trăm của điểm đang đặt chuột so với toàn bộ dải sóng
            const ratio = (scrollLeft + mouseX) / oldTotalWidth;
            // Vẽ lại sóng âm với kích thước mới
            renderWaveform(currentZoomLevel);
            // Lấy kích thước mới và điều chỉnh thanh cuộn để giữ điểm đó dưới con trỏ chuột
            const newTotalWidth = UI.waveformInner.offsetWidth;
            UI.waveformContainer.scrollLeft = (newTotalWidth * ratio) - mouseX;
        }
    } else {
        // Cuộn ngang bình thường khi lăn chuột
        UI.waveformContainer.scrollLeft += e.deltaY;
    }
});
// 5. Bấm lên waveform để tua (đồng hồ ảo)
UI.waveformInner.addEventListener('click', (e) => {
    const durationMs = getProjectDurationMs();
    if (durationMs <= 0) return;
    const rect = UI.waveformInner.getBoundingClientRect();
    const percentage = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    const targetMs = percentage * durationMs;
    const vc = ensureVirtualClock();
    const wasPlaying = vc.playing;
    if (wasPlaying) stopVirtualClock();
    vc.baseMs = targetMs;
    if (window.CanvasCompositor && window.CanvasCompositor.invalidateScene) {
        window.CanvasCompositor.invalidateScene('SEEK');
    }
    if (window.WasmBridge && window.WasmBridge.clock) {
        window.WasmBridge.clock.seek(targetMs);
    }
    updateFrame(targetMs);
    if (wasPlaying) startVirtualClock(targetMs);
});
// Cập nhật lại kích thước vẽ sóng âm khi đổi hướng màn hình hoặc resize (iPad, Responsive)
window.addEventListener('resize', () => {
    if (cachedAudioBuffer) renderWaveform(currentZoomLevel);
});
window.addEventListener('orientationchange', () => {
    setTimeout(() => {
        if (cachedAudioBuffer) renderWaveform(currentZoomLevel);
    }, 150);
});
