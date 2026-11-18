// ============================================================================
// EXPORT VIDEO — modal cấu hình, vẽ canvas, WebCodecs + Mediabunny
//   Phụ thuộc : ui.js, state.js, render-config.js, clock.js, preview.js, timeline-engine.js
//               và thư viện Mediabunny (global `Mediabunny`):
//               <script src="https://cdn.jsdelivr.net/npm/mediabunny/dist/bundles/mediabunny.min.js"></script>
//   Cung cấp  : drawCanvasFrame, drawCanvasFrameAsync, cancelVideoExport
//
// Cách hoạt động:
//   1. Thời lượng video = mốc `end` lớn nhất của timeline (không phụ thuộc độ dài file mp3).
//   2. Mỗi khung: đợi ảnh / video (seek) / font sẵn sàng -> vẽ canvas -> đưa cho Mediabunny.
//      Mediabunny lo chọn codec H.264/AAC, timestamp, xen kẽ audio-video, backpressure.
//   3. MP4 dựng trong RAM với moov ở ĐẦU file, ghi ra thư mục dự án MỘT lượt (không ghi rải rác).
//   4. File xuất ra được KIỂM TRA (cấu trúc box + nạp thử bằng <video>). Hỏng -> thử lại không audio
//      -> báo lỗi rõ ràng, không bao giờ báo "hoàn tất" với file hỏng.
//   5. Vòng lặp async tuần tự (không dùng requestAnimationFrame) -> không bị đứng khi chuyển tab.
//   6. `id` trong JSON không còn quan trọng: engine tự gán uid nội bộ cho mọi item.
// ============================================================================
function toggleExportDrawer() {
    if (state.isExporting) return;
    const drawer = document.getElementById('export-drawer');
    const aiDrawer = document.getElementById('ai-lyrics-drawer');
    const aiBtn = document.getElementById('ai-lyrics-btn');
    if (!drawer) return;
    const willOpen = !drawer.classList.contains('is-open');
    // Đóng drawer AI và Media nếu đang mở (đảm bảo gọn gàng, không tràn màn hình)
    const mediaDrawer = document.getElementById('media-drawer');
    const mediaBtn = document.getElementById('media-btn');
    const styleDrawer = document.getElementById('style-drawer');
    const styleBtn = document.getElementById('style-btn');
    if (willOpen) {
        if (aiDrawer) aiDrawer.classList.remove('is-open');
        if (aiBtn) aiBtn.classList.remove('is-active');
        if (mediaDrawer) mediaDrawer.classList.remove('is-open');
        if (mediaBtn) mediaBtn.classList.remove('is-active');
        if (styleDrawer) styleDrawer.classList.remove('is-open');
        if (styleBtn) {
            styleBtn.classList.remove('is-active');
            styleBtn.setAttribute('aria-expanded', 'false');
        }
    }
    drawer.classList.toggle('is-open', willOpen);
    if (UI.exportBtn) UI.exportBtn.classList.toggle('is-active', willOpen);
    setTimeout(() => {
        if (typeof refreshViewport === 'function') refreshViewport();
    }, 280);
}
function closeExportDrawer() {
    const drawer = document.getElementById('export-drawer');
    if (drawer) drawer.classList.remove('is-open');
    if (UI.exportBtn) UI.exportBtn.classList.remove('is-active');
    setTimeout(() => {
        if (typeof refreshViewport === 'function') refreshViewport();
    }, 280);
}
if (UI.exportBtn) {
    UI.exportBtn.addEventListener('click', toggleExportDrawer);
}
if (UI.cancelExportBtn) {
    UI.cancelExportBtn.addEventListener('click', closeExportDrawer);
}
// Gọi updateFrame (cập nhật preview) tối đa mỗi N ms trong lúc xuất. 0 = không gọi lần nào.
const EXPORT_PREVIEW_REFRESH_MS = 1000;
// Sai lệch thời lượng cho phép khi kiểm tra file (giây)
const EXPORT_VERIFY_TOLERANCE_S = 0.6;
// Máy yếu: ít nhân CPU hoặc ít RAM -> tắt cập nhật preview trong lúc xuất cho đỡ tốn CPU.
function isLowEndDevice() {
    const cores = navigator.hardwareConcurrency || 4;
    const mem = navigator.deviceMemory || 8;
    return cores <= 4 || mem <= 4;
}
/* =================================================================
   THỜI LƯỢNG
================================================================= */
// Thời lượng khi xuất = mốc end lớn nhất của timeline (hoặc config.durationMs nếu lớn hơn).
// Cố ý KHÔNG dùng getProjectDurationMs() của main.js (bản cũ lấy theo file mp3 / lyrics).
function exportDurationMs() {
    const tl = (window.TimelineEngine && state.timeline && state.timeline.length) ? TimelineEngine.getDurationMs() : 0;
    const cfg = state.config && Number(state.config.durationMs) > 0 ? Number(state.config.durationMs) : 0;
    const d = Math.max(tl, cfg);
    return d > 0 ? d : (typeof getProjectDurationMs === 'function' ? getProjectDurationMs() : 0);
}
/* =================================================================
   VẼ 1 KHUNG
================================================================= */
function paintExportFadeOut(ctx, wCanvas, hCanvas, timeMs, ec) {
    if (!ec.fadeOutEnabled) return;
    const durationMs = ec.durationMs !== undefined ? ec.durationMs : exportDurationMs();
    const fadeMs = Math.max(1, ec.fadeOutDuration || 0);
    const fadeStart = durationMs - fadeMs;
    if (durationMs > 0 && timeMs >= fadeStart) {
        const p = Math.min(1, (timeMs - fadeStart) / fadeMs);
        ctx.fillStyle = `rgba(0, 0, 0, ${p})`;
        ctx.fillRect(0, 0, wCanvas, hCanvas);
    }
}
function prepareExportCtx(ctx, wCanvas, hCanvas) {
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    // Nền mặc định: đen (item "background" trong timeline sẽ phủ lên)
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, wCanvas, hCanvas);
}
function exportEngineOpts() {
    return {
        baseW: state.renderConfig.baseW,
        baseH: state.renderConfig.baseH,
        fontFamily: state.renderConfig.fontFamily
    };
}
// Bản đồng bộ (giữ lại cho các nơi khác đang gọi). KHÔNG đợi media -> chỉ dùng cho preview/thumbnail.
function drawCanvasFrame(ctx, wCanvas, hCanvas, timeMs, ec) {
    prepareExportCtx(ctx, wCanvas, hCanvas);
    TimelineEngine.drawTextFrame(ctx, wCanvas, hCanvas, timeMs, exportEngineOpts());
    paintExportFadeOut(ctx, wCanvas, hCanvas, timeMs, ec);
}
// Bản dùng khi xuất: đợi ảnh/video/font sẵn sàng. Trả về số media không kịp sẵn sàng (0 = hoàn hảo).
async function drawCanvasFrameAsync(ctx, wCanvas, hCanvas, timeMs, ec) {
    prepareExportCtx(ctx, wCanvas, hCanvas);
    const notReady = await TimelineEngine.drawFrameAsync(ctx, wCanvas, hCanvas, timeMs, exportEngineOpts());
    paintExportFadeOut(ctx, wCanvas, hCanvas, timeMs, ec);
    return notReady || 0;
}
/* =================================================================
   TIỆN ÍCH
================================================================= */
let exportCancelled = false;   // người dùng bấm huỷ
function cancelVideoExport() { exportCancelled = true; }
window.cancelVideoExport = cancelVideoExport;
class ExportCancelled extends Error {
    constructor() { super('Đã huỷ xuất video'); this.name = 'ExportCancelled'; }
}
function checkExportAbort() {
    if (exportCancelled) throw new ExportCancelled();
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
// Nhường luồng chính bằng MessageChannel: KHÔNG bị trình duyệt làm chậm khi tab ở nền (khác setTimeout/rAF)
const yieldToBrowser = (() => {
    const ch = new MessageChannel();
    let waiting = [];
    ch.port1.onmessage = () => { const w = waiting; waiting = []; w.forEach(r => r()); };
    return () => new Promise(r => { waiting.push(r); ch.port2.postMessage(0); });
})();
// Quyền ghi vào thư mục dự án. Phải gọi NGAY khi click (còn user activation).
async function ensureWritable(dirHandle) {
    try {
        if (!dirHandle || !dirHandle.queryPermission) return false;
        const opts = { mode: 'readwrite' };
        if (await dirHandle.queryPermission(opts) === 'granted') return true;
        return (await dirHandle.requestPermission(opts)) === 'granted';
    } catch (e) {
        console.warn('Không xin được quyền ghi thư mục:', e);
        return false;
    }
}
async function saveExportedVideo(blob, fileName, canWriteDir) {
    if (canWriteDir && state.dirHandle) {
        try {
            const fh = await state.dirHandle.getFileHandle(fileName, { create: true });
            const w = await fh.createWritable();   // ghi vào file tạm, chỉ thay file thật khi close()
            await w.write(blob);
            await w.close();
            return 'folder';
        } catch (e) {
            console.error('Lưu video trực tiếp thất bại, chuyển sang tải xuống:', e);
        }
    }
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.hidden = true;
    a.href = url;
    a.download = fileName;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);   // revoke ngay sẽ làm hỏng tải xuống
    return 'download';
}
/* =================================================================
   KIỂM TRA FILE MP4 SAU KHI XUẤT
================================================================= */
// Đọc các box cấp cao nhất: phải có ftyp ở đầu, có moov và mdat, kích thước không vượt quá file.
function checkMp4Boxes(buf) {
    const dv = new DataView(buf), n = buf.byteLength;
    const boxes = [];
    let p = 0;
    while (p + 8 <= n) {
        let size = dv.getUint32(p), hdr = 8;
        const type = String.fromCharCode(dv.getUint8(p + 4), dv.getUint8(p + 5), dv.getUint8(p + 6), dv.getUint8(p + 7));
        if (size === 1) { size = Number(dv.getBigUint64(p + 8)); hdr = 16; }
        else if (size === 0) size = n - p;
        if (size < hdr || p + size > n) return { ok: false, why: `box "${type}" có kích thước sai (${size}) tại ${p}`, boxes };
        boxes.push(type);
        p += size;
    }
    if (boxes[0] !== 'ftyp') return { ok: false, why: 'file không bắt đầu bằng ftyp', boxes };
    if (!boxes.includes('moov')) return { ok: false, why: 'thiếu moov', boxes };
    if (!boxes.includes('mdat')) return { ok: false, why: 'thiếu mdat', boxes };
    return { ok: true, boxes };
}
// Nạp thử bằng <video>: đọc được metadata, thời lượng đúng, tua gần cuối được (bảng index hợp lệ).
function probeMp4WithVideoElement(blob, expectedS) {
    return new Promise(resolve => {
        const v = document.createElement('video');
        v.muted = true; v.preload = 'metadata';
        const url = URL.createObjectURL(blob);
        let timer = 0;
        const done = (ok, why) => {
            clearTimeout(timer);
            URL.revokeObjectURL(url);
            v.removeAttribute('src'); v.load();
            resolve({ ok, why });
        };
        timer = setTimeout(() => done(false, 'hết thời gian chờ nạp thử'), 15000);
        v.onerror = () => done(false, 'trình duyệt không mở được: ' + (v.error && v.error.message));
        v.onloadedmetadata = () => {
            if (!isFinite(v.duration) || Math.abs(v.duration - expectedS) > EXPORT_VERIFY_TOLERANCE_S) {
                return done(false, `thời lượng ${v.duration}s lệch so với ${expectedS.toFixed(2)}s`);
            }
            v.currentTime = Math.max(0, expectedS - 0.25);
        };
        v.onseeked = () => done(true, '');
        v.src = url;
    });
}
async function verifyExportedMp4(buffer, expectedS) {
    const boxes = checkMp4Boxes(buffer);
    if (!boxes.ok) return boxes;
    const probe = await probeMp4WithVideoElement(new Blob([buffer], { type: 'video/mp4' }), expectedS);
    return probe.ok ? { ok: true, boxes: boxes.boxes } : { ok: false, why: probe.why, boxes: boxes.boxes };
}
/* =================================================================
   TIỆN ÍCH UI OVERLAY XUẤT VIDEO (Preview nền đen + Viền tiến trình)
================================================================= */
let exportSvgTotalLength = 0;
function formatExportTime(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return '00:00';
    const s = Math.floor(seconds % 60);
    const m = Math.floor((seconds / 60) % 60);
    const h = Math.floor(seconds / 3600);
    const sStr = s < 10 ? '0' + s : '' + s;
    const mStr = m < 10 ? '0' + m : '' + m;
    if (h > 0) return `${h}:${mStr}:${sStr}`;
    return `${mStr}:${sStr}`;
}
function setupExportOverlayUI({ width, height, fps, bps, codec, durationMs, totalFrames, dirHandle, canWriteDir, fileName }) {
    const box = document.getElementById('export-viewport-box');
    const previewCanvas = document.getElementById('export-preview-canvas');
    const svg = document.getElementById('export-progress-svg');
    const track = document.getElementById('export-progress-track');
    const bar = document.getElementById('export-progress-bar');
    // 1. Tính toán kích thước preview box theo tỷ lệ khung hình thực tế
    const maxW = Math.min(560, Math.floor(window.innerWidth * 0.48));
    const maxH = Math.min(480, Math.floor(window.innerHeight * 0.54));
    const aspect = (width && height) ? (width / height) : (16 / 9);
    let prevW, prevH;
    if (aspect >= 1) {
        prevW = maxW;
        prevH = Math.round(prevW / aspect);
        if (prevH > maxH) {
            prevH = maxH;
            prevW = Math.round(prevH * aspect);
        }
    } else {
        prevH = maxH;
        prevW = Math.round(prevH * aspect);
        if (prevW > maxW) {
            prevW = maxW;
            prevH = Math.round(prevW / aspect);
        }
    }
    if (box) {
        box.style.width = `${prevW}px`;
        box.style.height = `${prevH}px`;
    }
    if (previewCanvas) {
        previewCanvas.width = width;
        previewCanvas.height = height;
    }
    // 2. Cài đặt SVG viền tiến trình góc vuông bao bọc quanh viewport (không bo tròn, bắt trọn 4 góc)
    const strokeW = 6;
    const svgW = prevW + strokeW * 2;
    const svgH = prevH + strokeW * 2;
    if (svg && track && bar) {
        svg.setAttribute('viewBox', `0 0 ${svgW} ${svgH}`);
        svg.style.width = `${svgW}px`;
        svg.style.height = `${svgH}px`;
        const rectX = strokeW / 2;
        const rectY = strokeW / 2;
        const rectW = svgW - strokeW;
        const rectH = svgH - strokeW;
        track.setAttribute('x', rectX);
        track.setAttribute('y', rectY);
        track.setAttribute('width', rectW);
        track.setAttribute('height', rectH);
        track.setAttribute('rx', 0);
        track.setAttribute('ry', 0);
        track.setAttribute('stroke-width', strokeW);
        bar.setAttribute('x', rectX);
        bar.setAttribute('y', rectY);
        bar.setAttribute('width', rectW);
        bar.setAttribute('height', rectH);
        bar.setAttribute('rx', 0);
        bar.setAttribute('ry', 0);
        bar.setAttribute('stroke-width', strokeW);
        try {
            exportSvgTotalLength = bar.getTotalLength();
        } catch (e) {
            exportSvgTotalLength = 2 * (rectW + rectH);
        }
        bar.style.strokeDasharray = `${exportSvgTotalLength} ${exportSvgTotalLength}`;
        bar.style.strokeDashoffset = `${exportSvgTotalLength}`;
    }
    // 3. Render danh sách thông số vào bảng bên phải bằng template Handlebars (spec-item)
    const codecMap = {
        av1: 'AV1 (AOMedia Video 1)',
        avc: 'H.264 (AVC)',
        hevc: 'H.265 (HEVC)',
        vp9: 'VP9'
    };
    const codecName = codecMap[codec] || String(codec).toUpperCase();
    const tag = height === 1080 ? ' (1080p)' : (height === 720 ? ' (720p)' : (height === 2160 ? ' (4K)' : ''));
    const mbps = Math.round(bps / 1000000);
    const specsData = [
        { key: 'res', label: 'Resolution', id: 'export-spec-res', value: `${width} × ${height}${tag}` },
        { key: 'fps', label: 'FPS', id: 'export-spec-fps', value: `${fps} FPS` },
        { key: 'codec', label: 'Algorithm', id: 'export-spec-codec', value: codecName },
        { key: 'bitrate', label: 'Bitrate', id: 'export-spec-bitrate', value: `${mbps} Mbps` },
        { key: 'frames', label: 'Frames', id: 'export-spec-frames', value: `0 / ${totalFrames} (0%)` },
        { key: 'eta', label: 'Remaining', id: 'export-spec-eta', value: 'Calculating...' },
        { key: 'elapsed', label: 'Elapsed', id: 'export-spec-elapsed', value: '00:00' }
    ];
    const specsList = document.querySelector('.export-specs-list');
    if (specsList && window.Templates && typeof window.Templates.render === 'function') {
        specsList.innerHTML = specsData.map(s => Templates.render('spec-item', s)).join('\n');
    } else {
        const resEl = document.getElementById('export-spec-res');
        if (resEl) resEl.textContent = `${width} × ${height}${tag}`;
        const fpsEl = document.getElementById('export-spec-fps');
        if (fpsEl) fpsEl.textContent = `${fps} FPS`;
        const codecEl = document.getElementById('export-spec-codec');
        if (codecEl) codecEl.textContent = codecName;
        const bitrateEl = document.getElementById('export-spec-bitrate');
        if (bitrateEl) bitrateEl.textContent = `${mbps} Mbps`;
        const framesEl = document.getElementById('export-spec-frames');
        if (framesEl) framesEl.textContent = `0 / ${totalFrames} (0%)`;
        const etaEl = document.getElementById('export-spec-eta');
        if (etaEl) etaEl.textContent = `Calculating...`;
        const elapsedEl = document.getElementById('export-spec-elapsed');
        if (elapsedEl) elapsedEl.textContent = `00:00`;
    }
    // 4. Badge thời gian sử dụng [time use] góc dưới phải preview
    const timeBadge = document.getElementById('export-time-badge');
    if (timeBadge) {
        timeBadge.textContent = `00:00 / ${formatExportTime(durationMs / 1000)}`;
    }
    // 5. Đường dẫn lưu video ở footer dưới preview
    const savePathEl = document.getElementById('export-save-path');
    if (savePathEl) {
        if (canWriteDir && dirHandle && dirHandle.name) {
            savePathEl.textContent = `${dirHandle.name}/${fileName}`;
            savePathEl.title = `Output path: ${dirHandle.name}/${fileName}`;
        } else {
            savePathEl.textContent = `Download Folder/${fileName}`;
            savePathEl.title = `Download via browser: ${fileName}`;
        }
    }
    // 6. Reset trạng thái nút Tạm dừng & Hủy
    isExportPaused = false;
    totalPausedTime = 0;
    const pauseBtn = document.getElementById('export-pause-btn');
    if (pauseBtn) {
        pauseBtn.textContent = 'Pause';
        pauseBtn.classList.remove('is-paused');
        pauseBtn.disabled = false;
    }
    const cancelBtn = document.getElementById('export-cancel-btn');
    if (cancelBtn) {
        cancelBtn.textContent = 'Cancel';
        cancelBtn.disabled = false;
    }
}
let isExportPaused = false;
let totalPausedTime = 0;
function toggleExportPause() {
    if (!state.isExporting) return;
    isExportPaused = !isExportPaused;
    const pauseBtn = document.getElementById('export-pause-btn');
    if (pauseBtn) {
        if (isExportPaused) {
            pauseBtn.textContent = '▶ Tiếp tục';
            pauseBtn.classList.add('is-paused');
        } else {
            pauseBtn.textContent = 'Tạm dừng';
            pauseBtn.classList.remove('is-paused');
        }
    }
    const etaEl = document.getElementById('export-spec-eta');
    if (etaEl && isExportPaused) {
        etaEl.textContent = 'Đang tạm dừng';
    }
}
function updateExportProgressUI({ currentFrame, totalFrames, timeMs, durationMs, startTime }) {
    const progress = Math.max(0, Math.min(1, currentFrame / totalFrames));
    // Viền tiến trình SVG
    const bar = document.getElementById('export-progress-bar');
    if (bar && exportSvgTotalLength > 0) {
        bar.style.strokeDashoffset = `${exportSvgTotalLength * (1 - progress)}`;
    }
    // Số khung hình và %
    const framesEl = document.getElementById('export-spec-frames');
    if (framesEl) {
        framesEl.textContent = `${currentFrame} / ${totalFrames} (${Math.round(progress * 100)}%)`;
    }
    // Thời gian đã chạy (trừ đi thời gian đã tạm dừng) & ETA
    const elapsedS = Math.max(0, (performance.now() - startTime - totalPausedTime) / 1000);
    const elapsedEl = document.getElementById('export-spec-elapsed');
    if (elapsedEl) {
        elapsedEl.textContent = formatExportTime(elapsedS);
    }
    const etaEl = document.getElementById('export-spec-eta');
    if (etaEl && !isExportPaused) {
        if (currentFrame >= 6 && progress < 1) {
            const avgSecPerFrame = elapsedS / currentFrame;
            const remainFrames = totalFrames - currentFrame;
            const etaS = Math.max(0, Math.round(remainFrames * avgSecPerFrame));
            etaEl.textContent = etaS < 60 ? `~${etaS}s` : `~${formatExportTime(etaS)}`;
        } else if (progress >= 1) {
            etaEl.textContent = 'Hoàn tất';
        }
    }
    // Badge [time use] góc dưới phải khung preview
    const timeBadge = document.getElementById('export-time-badge');
    if (timeBadge) {
        timeBadge.textContent = `${formatExportTime(timeMs / 1000)} / ${formatExportTime(durationMs / 1000)}`;
    }
    if (UI.exportProgress) {
        UI.exportProgress.style.setProperty('--progress', progress * 100);
    }
}
// Xử lý sự kiện nút Tạm dừng & Hủy
const exportPauseBtn = document.getElementById('export-pause-btn');
if (exportPauseBtn) {
    exportPauseBtn.addEventListener('click', toggleExportPause);
}
const exportCancelBtn = document.getElementById('export-cancel-btn');
if (exportCancelBtn) {
    exportCancelBtn.addEventListener('click', () => {
        cancelVideoExport();
        exportCancelBtn.textContent = 'Đang hủy...';
        exportCancelBtn.disabled = true;
    });
}
function onExportKeyDown(e) {
    if (e.key === 'Escape' && state.isExporting) {
        cancelVideoExport();
        const btn = document.getElementById('export-cancel-btn');
        if (btn) {
            btn.textContent = 'Đang hủy...';
            btn.disabled = true;
        }
    } else if (e.code === 'Space' && state.isExporting && e.target.tagName !== 'INPUT' && e.target.tagName !== 'TEXTAREA') {
        e.preventDefault();
        toggleExportPause();
    }
}
/* =================================================================
   MỘT LẦN MÃ HOÁ (Mediabunny)
================================================================= */
async function encodeVideoOnce({ withAudio, audioBuffer, canvas, ctx, fps, bps, codec, durationMs, exportConfig }) {
    const M = window.Mediabunny;
    const output = new M.Output({
        format: new M.Mp4OutputFormat({ fastStart: 'in-memory' }),   // moov ở đầu file
        target: new M.BufferTarget()
    });
    // Thuật toán nén: AV1 (mạnh nhất hiện nay), H.264 (AVC), H.265 (HEVC), VP9
    const selectedCodec = codec || 'avc';
    let videoSource;
    try {
        videoSource = new M.CanvasSource(canvas, { codec: selectedCodec, bitrate: bps, keyFrameInterval: 2 });
    } catch (e) {
        console.warn(`[Export] Codec "${selectedCodec}" không tương thích, tự động đổi về H.264 (avc):`, e);
        videoSource = new M.CanvasSource(canvas, { codec: 'avc', bitrate: bps, keyFrameInterval: 2 });
    }
    output.addVideoTrack(videoSource, { frameRate: fps });
    let audioSource = null;
    if (withAudio && audioBuffer) {
        audioSource = new M.AudioBufferSource({ codec: 'aac', bitrate: 192000 });
        output.addAudioTrack(audioSource);
    }
    try {
        await output.start();
        // Audio chạy song song với video để Mediabunny xen kẽ hai track đúng thứ tự
        let audioError = null;
        const audioDone = audioSource
            ? audioSource.add(audioBuffer).then(() => audioSource.close()).catch(e => { audioError = e; })
            : Promise.resolve();
        const totalFrames = Math.max(1, Math.ceil((durationMs / 1000) * fps));
        let notReadyTotal = 0, lastYield = performance.now();
        const startTime = performance.now();
        const previewCanvas = document.getElementById('export-preview-canvas');
        const pctx = previewCanvas ? previewCanvas.getContext('2d') : null;
        const lastValidCanvas = document.createElement('canvas');
        lastValidCanvas.width = canvas.width;
        lastValidCanvas.height = canvas.height;
        const lastValidCtx = lastValidCanvas.getContext('2d', { alpha: false });
        let hasValidFrame = false;
        for (let f = 0; f < totalFrames; f++) {
            checkExportAbort();
            if (audioError) throw audioError;
            // Xử lý tạm dừng xuất video (cho phép bấm Hủy ngay trong lúc dừng)
            if (isExportPaused) {
                const pauseStart = performance.now();
                const etaEl = document.getElementById('export-spec-eta');
                if (etaEl) etaEl.textContent = 'Đang tạm dừng';
                while (isExportPaused) {
                    checkExportAbort();
                    await sleep(80);
                }
                totalPausedTime += (performance.now() - pauseStart);
            }
            const timeMs = (f * 1000) / fps;
            // Vẽ frame hiện tại
            const notReady = await drawCanvasFrameAsync(ctx, canvas.width, canvas.height, timeMs, exportConfig);
            if (notReady > 0 && hasValidFrame) {
                // [NỘI SUY / GHI ĐÈ]: Khung này bị thiếu media -> đè lại bằng frame hợp lệ trước đó
                ctx.drawImage(lastValidCanvas, 0, 0);
            } else {
                // Khung này hợp lệ -> lưu lại làm frame mẫu cho các khung sau
                lastValidCtx.drawImage(canvas, 0, 0);
                hasValidFrame = true;
            }
            // [LIVE PREVIEW]: Cập nhật frame hiện tại lên khung preview trên overlay
            if (pctx) {
                pctx.drawImage(canvas, 0, 0, previewCanvas.width, previewCanvas.height);
            }
            // [TIẾN TRÌNH & THÔNG SỐ]: Cập nhật viền tiến trình, số frame, ETA, time badge
            const isLast = f === totalFrames - 1;
            if (f === 0 || isLast || (f % 2 === 0)) {
                updateExportProgressUI({
                    currentFrame: f + 1,
                    totalFrames,
                    timeMs,
                    durationMs,
                    startTime
                });
            }
            notReadyTotal += notReady;
            await videoSource.add(f / fps, 1 / fps);   // Đưa frame vào bộ mã hoá
            if (performance.now() - lastYield > 40) { await yieldToBrowser(); lastYield = performance.now(); }
        }
        videoSource.close();
        await audioDone;
        if (audioError) throw audioError;
        checkExportAbort();
        await output.finalize();
        return { buffer: output.target.buffer, notReadyTotal, totalFrames };
    } catch (err) {
        try { await output.cancel(); } catch (e) { /* đã đóng */ }
        throw err;
    }
}
/* =================================================================
   XUẤT VIDEO (dựng canvas, âm thanh, thử lại, kiểm tra)
================================================================= */
async function exportVideo(opts = {}) {
    if (!window.Mediabunny) throw new Error('Thiếu thư viện Mediabunny (thêm thẻ <script> trước export.js).');
    if (typeof VideoEncoder === 'undefined' || typeof VideoFrame === 'undefined') {
        throw new Error('Trình duyệt không hỗ trợ WebCodecs. Hãy dùng Chrome hoặc Edge bản mới.');
    }
    const fps = opts.fps || Math.max(1, Math.min(240, parseInt(UI.exportFps ? UI.exportFps.value : 60) || 60));
    const bps = opts.bps || (parseInt(UI.exportBitrate ? UI.exportBitrate.value : 30000000) || 15000000);
    const codec = opts.codec || ((UI.exportCodec ? UI.exportCodec.value : 'av1') || 'av1');
    const durationMs = opts.durationMs || exportDurationMs();
    if (!(durationMs > 0)) throw new Error('Dự án không có thời lượng để xuất.');
    let canvas = opts.canvas || UI.exportCanvas;
    if (!opts.canvas) {
        const targetHeight = Math.min(2160, parseInt(UI.exportRes ? UI.exportRes.value : 1080) || 1080);
        if (!state.renderConfig) await buildRenderConfig();
        const rc = state.renderConfig;
        if (rc.ratioW >= rc.ratioH) {
            canvas.height = targetHeight;
            canvas.width = Math.floor(targetHeight * (rc.ratioW / rc.ratioH) / 2) * 2;
        } else {
            canvas.width = Math.floor(targetHeight / 2) * 2;
            canvas.height = Math.floor(targetHeight * (rc.ratioH / rc.ratioW) / 2) * 2;
        }
        canvas.width += canvas.width % 2;
        canvas.height += canvas.height % 2;
    }
    const ctx = canvas.getContext('2d', { alpha: false });
    if (!state.renderConfig) await buildRenderConfig();
    const rc = state.renderConfig;
    const exportConfig = { fadeOutEnabled: !!rc.fadeOutEnabled, fadeOutDuration: rc.fadeOutDuration, durationMs };
    // Âm thanh: dựng 1 lần (đã gồm đường cong volume / fade của item audio)
    let audioBuffer = null, audioNote = '';
    try {
        audioBuffer = await TimelineEngine.buildHighResAudioBuffer(durationMs);
    } catch (e) {
        console.warn('Không dựng được âm thanh:', e);
        audioNote = 'Không dựng được âm thanh, video xuất ra không có tiếng.';
    }
    checkExportAbort();
    const attempts = audioBuffer ? [{ withAudio: true }, { withAudio: false }] : [{ withAudio: false }];
    const errors = [];
    for (const at of attempts) {
        try {
            try { TimelineEngine.disposeExport(); } catch (e) { /* bỏ qua */ }
            if (UI.exportProgress) UI.exportProgress.style.setProperty('--progress', 0);
            const r = await encodeVideoOnce({ ...at, audioBuffer, canvas, ctx, fps, bps, codec, durationMs, exportConfig });
            const check = await verifyExportedMp4(r.buffer, durationMs / 1000);
            console.info('[Export]', { ...at, frames: r.totalFrames, bytes: r.buffer.byteLength, boxes: check.boxes, verify: check });
            if (!check.ok) throw new Error('File xuất ra không hợp lệ: ' + check.why);
            if (!at.withAudio && audioBuffer) audioNote = 'Mã hoá âm thanh lỗi trên máy này nên video xuất ra không có tiếng.';
            return { blob: new Blob([r.buffer], { type: 'video/mp4' }), notReadyTotal: r.notReadyTotal, audioNote, totalFrames: r.totalFrames };
        } catch (e) {
            if (e instanceof ExportCancelled) throw e;
            console.warn('[Export] Lần thử thất bại:', at, e);
            errors.push(`${at.withAudio ? 'có audio' : 'không audio'}: ${e && e.message ? e.message : e}`);
        }
    }
    throw new Error('Xuất video thất bại.\n' + errors.join('\n'));
}
/* =================================================================
   NÚT XUẤT
================================================================= */
UI.startExportBtn.addEventListener('click', async () => {
    if (state.isExporting) return;
    closeExportDrawer();
    // Phải xin quyền NGAY (còn user activation), trước mọi thao tác dài
    const canWriteDir = await ensureWritable(state.dirHandle);
    const targetHeight = Math.min(2160, parseInt(UI.exportRes ? UI.exportRes.value : 1080) || 1080);
    const fps = Math.max(1, Math.min(240, parseInt(UI.exportFps ? UI.exportFps.value : 60) || 60));
    const bps = parseInt(UI.exportBitrate ? UI.exportBitrate.value : 30000000) || 15000000;
    const codec = (UI.exportCodec ? UI.exportCodec.value : 'av1') || 'av1';
    if (!state.renderConfig) await buildRenderConfig();
    const rc = state.renderConfig;
    // Kích thước canvas: chẵn (H.264 yêu cầu)
    const canvas = UI.exportCanvas;
    if (rc.ratioW >= rc.ratioH) {
        canvas.height = targetHeight;
        canvas.width = Math.floor(targetHeight * (rc.ratioW / rc.ratioH) / 2) * 2;
    } else {
        canvas.width = Math.floor(targetHeight / 2) * 2;
        canvas.height = Math.floor(targetHeight * (rc.ratioH / rc.ratioW) / 2) * 2;
    }
    canvas.width += canvas.width % 2;
    canvas.height += canvas.height % 2;
    const durationMs = exportDurationMs();
    if (!(durationMs > 0)) {
        alert('Dự án không có thời lượng để xuất.');
        return;
    }
    const totalFrames = Math.max(1, Math.ceil((durationMs / 1000) * fps));
    const safeName = String((state.dirHandle && state.dirHandle.name) || 'timeline').replace(/[\\/:*?"<>|]/g, '_');
    const fileName = `${safeName}.mp4`;
    // Cài đặt giao diện Overlay xuất video trước khi mở
    setupExportOverlayUI({
        width: canvas.width,
        height: canvas.height,
        fps,
        bps,
        codec,
        durationMs,
        totalFrames,
        dirHandle: state.dirHandle,
        canWriteDir,
        fileName
    });
    UI.loadingOverlay.classList.add('is-open');
    window.addEventListener('keydown', onExportKeyDown);
    state.isExporting = true;
    exportCancelled = false;
    // Giữ màn hình/máy không ngủ trong lúc xuất
    let wakeLock = null;
    try { if (navigator.wakeLock) wakeLock = await navigator.wakeLock.request('screen'); } catch (e) { /* không bắt buộc */ }
    stopVirtualClock();
    cancelAnimationFrame(state.animationFrameId);
    let result = null, error = null;
    try {
        result = await exportVideo({ canvas, fps, bps, codec, durationMs, totalFrames });
    } catch (e) {
        error = e;
    }
    // Dọn dẹp: luôn giải phóng stage export (video element, canvas phụ)
    try { TimelineEngine.disposeExport(); } catch (e) { console.warn(e); }
    let message = '';
    if (result) {
        try {
            const where = await saveExportedVideo(result.blob, fileName, canWriteDir);
            message = where === 'folder'
                ? `🎉 Xuất video hoàn tất!\nVideo đã được lưu trực tiếp vào thư mục: ${state.dirHandle.name}\nTên file: ${fileName}`
                : `🎉 Xuất video hoàn tất!\nVideo đã được tải xuống: ${fileName}`;
            if (result.audioNote) message += `\n⚠️ ${result.audioNote}`;
            if (result.notReadyTotal > 0) {
                message += `\n⚠️ ${result.notReadyTotal} lần ảnh/video chưa tải kịp (đã bỏ qua ở vài khung). Hãy kiểm tra file media trong thư mục dự án.`;
            }
        } catch (e) {
            error = e;
        }
    }
    try { if (wakeLock) await wakeLock.release(); } catch (e) { /* bỏ qua */ }
    window.removeEventListener('keydown', onExportKeyDown);
    state.isExporting = false;
    UI.loadingOverlay.classList.remove('is-open');
    if (error) {
        if (error instanceof ExportCancelled) return;   // người dùng tự huỷ, không báo lỗi
        console.error('Xuất video thất bại:', error);
        alert('Xuất video thất bại: ' + (error && error.message ? error.message : error));
    } else if (message) {
        alert(message);
    }
});
