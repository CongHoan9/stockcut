// ============================================================================
// js/ai-lyrics.js — Tích hợp AI Tách Vocal & Sinh Lyrics chuẩn Timeline
// ============================================================================
(function () {
    'use strict';
    const AI_API_URL = 'http://127.0.0.1:8000';
    // DOM Elements
    let aiBtn, aiDrawer, aiStartBtn;
    let aiAudioFileName, aiSeparateVocalsCheck, aiAppendOnlyCheck, aiLanguageSelect, aiModelSelect;
    let aiLyricsTextarea, aiErrorMsg;
    let aiFilePicker, aiChooseFileBtn;
    let aiProgressContainer, aiProgressFill, aiProgressStatus, aiProgressPercent;
    let selectedCustomFile = null;
    function initElements() {
        aiBtn = document.getElementById('ai-lyrics-btn');
        aiDrawer = document.getElementById('ai-lyrics-drawer');
        aiStartBtn = document.getElementById('ai-start-btn');
        aiAudioFileName = document.getElementById('ai-audio-file-name');
        aiSeparateVocalsCheck = document.getElementById('ai-separate-vocals');
        aiAppendOnlyCheck = document.getElementById('ai-append-only');
        aiLanguageSelect = document.getElementById('ai-language');
        aiModelSelect = document.getElementById('ai-model');
        aiLyricsTextarea = document.getElementById('ai-lyrics-text');
        aiErrorMsg = document.getElementById('ai-error-msg');
        aiFilePicker = document.getElementById('ai-file-picker');
        aiChooseFileBtn = document.getElementById('ai-choose-file-btn');
        // Thanh ngang tiến trình khi đang chạy
        aiProgressContainer = document.getElementById('ai-progress-container');
        aiProgressFill = document.getElementById('ai-progress-fill');
        aiProgressStatus = document.getElementById('ai-progress-status');
        aiProgressPercent = document.getElementById('ai-progress-percent');
        if (aiChooseFileBtn && aiFilePicker) {
            aiChooseFileBtn.onclick = () => aiFilePicker.click();
            if (aiAudioFileName) {
                aiAudioFileName.onclick = () => aiFilePicker.click();
            }
            aiFilePicker.onchange = (e) => {
                if (e.target.files && e.target.files[0]) {
                    selectedCustomFile = e.target.files[0];
                    if (aiAudioFileName) {
                        aiAudioFileName.innerText = `${selectedCustomFile.name} (${(selectedCustomFile.size / (1024 * 1024)).toFixed(2)} MB)`;
                    }
                }
            };
        }
    }
    // Cập nhật thanh ngang tiến trình (Chỉ hiển thị khi đang chạy)
    function updateProgressBar(pct, statusText, isError = false) {
        initElements();
        if (!aiProgressContainer) return;
        if (pct <= 0 && !statusText) {
            aiProgressContainer.style.display = 'none';
            return;
        }
        aiProgressContainer.style.display = 'grid';
        const clamped = Math.min(100, Math.max(0, pct));
        if (aiProgressFill) {
            aiProgressFill.style.width = `${clamped}%`;
            aiProgressFill.style.background = isError 
                ? '#ef4444' 
                : 'linear-gradient(90deg, #43ad6a, #3b82f6, #4ade80)';
        }
        if (aiProgressPercent) {
            aiProgressPercent.innerText = `${clamped}%`;
            aiProgressPercent.style.color = isError ? '#ef4444' : '#4ade80';
        }
        if (aiProgressStatus && statusText) {
            aiProgressStatus.innerText = statusText;
            aiProgressStatus.style.color = isError ? '#f87171' : '#c4b5fd';
        }
    }
    // Kiểm tra kết nối AI Backend Server
    async function checkServerStatus() {
        initElements();
        try {
            const res = await fetch(`${AI_API_URL}/api/status`, { signal: AbortSignal.timeout(3000) });
            if (res.ok) {
                if (aiStartBtn) aiStartBtn.disabled = false;
                if (aiErrorMsg) aiErrorMsg.innerText = '';
                return true;
            }
        } catch (e) {
            // Server chưa bật
        }
        if (aiErrorMsg) {
            aiErrorMsg.innerText = 'Máy chủ AI offline (chạy python service_ai.py để bật)';
        }
        return false;
    }
    // Tìm file audio trong project hiện tại
    async function getProjectAudioFile() {
        if (!state.dirHandle) return null;
        const audioItem = state.timeline.find(it => (it.type === 'audio' || it.type === 'video') && it.src);
        const candidates = audioItem ? [audioItem.src, 'audio.mp3', 'audio.m4a', 'audio.wav'] : ['audio.mp3', 'audio.m4a', 'audio.wav'];
        for (const name of candidates) {
            try {
                const fh = await state.dirHandle.getFileHandle(name);
                return await fh.getFile();
            } catch (_) {}
        }
        return null;
    }
    // Tắt mở Drawer inline (Slide-down / Collapse)
    async function toggleDrawer() {
        initElements();
        if (!aiDrawer) return;
        const willOpen = !aiDrawer.classList.contains('is-open');
        // Tự động đóng Export drawer và Media drawer nếu đang mở (giữ không gian thoáng đãng)
        const exportDrawer = document.getElementById('export-drawer');
        const mediaDrawer = document.getElementById('media-drawer');
        const styleDrawer = document.getElementById('style-drawer');
        const mediaBtn = document.getElementById('media-btn');
        const styleBtn = document.getElementById('style-btn');
        if (willOpen) {
            if (exportDrawer) {
                exportDrawer.classList.remove('is-open');
                if (UI.exportBtn) UI.exportBtn.classList.remove('is-active');
            }
            if (mediaDrawer) {
                mediaDrawer.classList.remove('is-open');
                if (mediaBtn) mediaBtn.classList.remove('is-active');
            }
            if (styleDrawer) styleDrawer.classList.remove('is-open');
            if (styleBtn) {
                styleBtn.classList.remove('is-active');
                styleBtn.setAttribute('aria-expanded', 'false');
            }
        }
        aiDrawer.classList.toggle('is-open', willOpen);
        if (aiBtn) aiBtn.classList.toggle('is-active', willOpen);
        if (willOpen) {
            if (aiErrorMsg) aiErrorMsg.innerText = '';
            const audioFile = selectedCustomFile || await getProjectAudioFile();
            if (aiAudioFileName) {
                if (audioFile) {
                    aiAudioFileName.innerText = `${audioFile.name} (${(audioFile.size / (1024 * 1024)).toFixed(2)} MB)`;
                } else {
                    aiAudioFileName.innerText = 'Chưa tìm thấy audio trong thư mục hiện tại';
                }
            }
            if (aiLyricsTextarea && !aiLyricsTextarea.value.trim()) {
                if (state.ram && state.ram.lyricsTxt) {
                    aiLyricsTextarea.value = state.ram.lyricsTxt;
                } else if (state.lyricsFileHandle) {
                    state.lyricsFileHandle.getFile().then(f => f.text()).then(txt => {
                        if (txt && aiLyricsTextarea && !aiLyricsTextarea.value.trim()) {
                            aiLyricsTextarea.value = txt;
                            if (state.ram) state.ram.lyricsTxt = txt;
                        }
                    }).catch(() => {});
                }
            }
            checkServerStatus();
        }
        setTimeout(() => {
            if (typeof refreshViewport === 'function') refreshViewport();
        }, 280);
    }
    function closeDrawer() {
        initElements();
        if (aiDrawer) aiDrawer.classList.remove('is-open');
        if (aiBtn) aiBtn.classList.remove('is-active');
        setTimeout(() => {
            if (typeof refreshViewport === 'function') refreshViewport();
        }, 280);
    }
    // Xử lý gửi yêu cầu sinh lyrics
    async function handleStartAILyrics() {
        initElements();
        if (aiErrorMsg) aiErrorMsg.innerText = '';
        const audioFile = selectedCustomFile || await getProjectAudioFile();
        if (!audioFile) {
            if (aiErrorMsg) aiErrorMsg.innerText = 'Lỗi: Không tìm thấy file âm thanh. Vui lòng bấm "Đổi file" để chọn file từ máy.';
            return;
        }
        if (aiStartBtn) aiStartBtn.disabled = true;
        // Bắt đầu: Hiển thị thanh tiến trình 15%
        updateProgressBar(15, 'Đang tải file audio & khởi chạy AI...');
        try {
            const formData = new FormData();
            formData.append('file', audioFile);
            formData.append('separate_vocals', aiSeparateVocalsCheck && aiSeparateVocalsCheck.checked ? 'true' : 'false');
            formData.append('language', aiLanguageSelect ? aiLanguageSelect.value : 'auto');
            formData.append('model_size', aiModelSelect ? aiModelSelect.value : 'base');
            const userText = aiLyricsTextarea ? aiLyricsTextarea.value.trim() : '';
            if (userText) {
                formData.append('lyrics_text', userText);
            }
            // Cập nhật các giai đoạn phân tích trên thanh tiến trình
            let timer1, timer2;
            if (aiSeparateVocalsCheck && aiSeparateVocalsCheck.checked) {
                timer1 = setTimeout(() => {
                    updateProgressBar(45, 'Đang tách vocal sạch (Demucs AI)...');
                }, 2000);
                timer2 = setTimeout(() => {
                    updateProgressBar(75, 'Đang nhận diện giọng nói (Whisper AI)...');
                }, 7000);
            } else {
                timer1 = setTimeout(() => {
                    updateProgressBar(65, 'Đang nhận diện giọng nói (Whisper AI)...');
                }, 2000);
            }
            const response = await fetch(`${AI_API_URL}/api/process-lyrics`, {
                method: 'POST',
                body: formData
            });
            if (timer1) clearTimeout(timer1);
            if (timer2) clearTimeout(timer2);
            if (!response.ok) {
                const errData = await response.json().catch(() => ({}));
                throw new Error(errData.detail || `Server trả về lỗi: ${response.status}`);
            }
            const data = await response.json();
            // Hoàn tất 100%
            updateProgressBar(100, `Hoàn tất! Đã tạo ${data.total_items} câu lyric.`);
            if (!data.timeline_items || !data.timeline_items.length) {
                throw new Error('AI không nhận diện được lời trong đoạn âm thanh này.');
            }
            // ================================================================
            // CHỈ THÊM LYRIC VÀO JSON CÓ SẴN (GIỮ NGUYÊN TOÀN BỘ CẤU TRÚC GỐC)
            // ================================================================
            let currentData = {};
            try {
                currentData = JSON.parse(state.ram.projectJson || UI.jsonEditor.value);
            } catch (e) {
                // Fallback nếu JSON trên editor đang trống hoặc chưa parse được
                currentData = {
                    config: state.config || { aspectRatio: "9:16" },
                    timeline: Array.isArray(state.timeline) ? state.timeline : []
                };
            }
            if (!Array.isArray(currentData.timeline)) {
                currentData.timeline = [];
            }
            // Đảm bảo mọi item lyric AI sinh ra đều chuẩn mẫu hoàn hảo: 50cqw, 50cqh, textAlign center, add, translate(-50%, -50%), revealDuration 100
            const newLyricItems = (data.timeline_items || []).map(it => ({
                type: 'lyric',
                start: it.start,
                end: it.end,
                left: it.left || '50cqw',
                top: it.top || '50cqh',
                textAlign: it.textAlign || 'center',
                revealMode: it.revealMode || 'add',
                transform: it.transform || 'translate(-50%, -50%)',
                revealDuration: it.revealDuration !== undefined ? it.revealDuration : 100,
                keyframes: it.keyframes || []
            }));
            // XÓA MỌI TYPE 'lyric' KHI SINH LYRIC:
            // Tuyệt đối giữ nguyên 100% type 'text' (watermark, tiêu đề, credit...),
            // CD, Effect, Background, Video, Audio...
            currentData.timeline = currentData.timeline.filter(it => {
                if (it.type === 'lyric') return false; // Xóa toàn bộ type lyric
                // Dọn sạch các câu lyric cũ của bản trước nếu còn lưu dạng text + revealMode
                if (it.type === 'text' && it.revealMode && it.keyframes && it.keyframes.length > 0 && (it.top === '50cqh' || it.top === '68cqh')) return false;
                return true; // text và các item khác được giữ nguyên 100%
            });
            currentData.timeline.push(...newLyricItems);
            // Đồng bộ lại vào RAM và editor với dữ liệu gốc đã được chèn thêm lyric
            const updatedJsonStr = JSON.stringify(currentData, null, " ");
            state.ram.projectJson = updatedJsonStr;
            state.ram.isJsonDirty = true;
            UI.jsonEditor.value = updatedJsonStr;
            UI.jsonEditor.dispatchEvent(new Event('input'));
            // Không tự động ghi đè file đĩa: dữ liệu được lưu tạm trong RAM,
            // chỉ khi người dùng ấn Save (Ctrl+S) mới ghi ra file thật trên đĩa
            UI.statusText.innerText = `✓ Đã sinh ${data.total_items} câu lyric AI vào RAM! (Nhấn Ctrl+S để lưu ra file)`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => {
                closeDrawer();
                if (aiStartBtn) aiStartBtn.disabled = false;
                setTimeout(() => updateProgressBar(0, ''), 500);
            }, 1200);
        } catch (err) {
            console.error('[AI Lyrics Error]:', err);
            if (aiErrorMsg) aiErrorMsg.innerText = `Lỗi: ${err.message}`;
            updateProgressBar(0, 'Thất bại!', true);
            if (aiStartBtn) aiStartBtn.disabled = false;
        }
    }
    // Khởi tạo các sự kiện khi trang đã tải
    window.addEventListener('DOMContentLoaded', () => {
        initElements();
        if (aiBtn) aiBtn.addEventListener('click', toggleDrawer);
        if (aiStartBtn) aiStartBtn.addEventListener('click', handleStartAILyrics);
    });
    // Xuất ra global nếu cần gọi thủ công
    window.AILyrics = {
        toggleDrawer,
        closeDrawer,
        openModal: toggleDrawer,
        closeModal: closeDrawer,
        checkServerStatus
    };
})();
