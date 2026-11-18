// ============================================================================
// UI — tham chiếu DOM (chỉ lấy phần tử, KHÔNG tạo markup, KHÔNG gán style)
//   Markup nằm ở index.html, diện mạo nằm ở css/*.css.
//   JS chỉ: đổi class (.is-open, .is-playing, .is-ready, .is-error ...),
//           hoặc truyền SỐ cho CSS qua biến (--vp-w, --progress, --fade ...).
// ============================================================================
const UI = window.UI = {
    // Thanh điều khiển trên cùng
    loadBtn: document.getElementById('load-btn'),
    exportBtn: document.getElementById('export-btn'),
    mediaBtn: document.getElementById('media-btn'),
    styleBtn: document.getElementById('style-btn'),
    fullscreenBtn: document.getElementById('fullscreen-btn'),
    statusText: document.getElementById('status-text'),
    // Khung render (Kiến trúc Native Canvas Compositor 100% - duy nhất 1 canvas)
    viewport: document.getElementById('render-viewport'),
    fullscreenStage: document.getElementById('fullscreen-stage'),
    engineCanvas: document.getElementById('engine-canvas'),
    // Fallbacks an toàn cho các module cũ (không gắn vào DOM, tránh lỗi null access)
    overlayLayer: document.getElementById('render-viewport') || (typeof document !== 'undefined' ? document.createElement('div') : null),
    lyricContainer: (typeof document !== 'undefined' ? document.createElement('div') : null),
    watermark: (typeof document !== 'undefined' ? document.createElement('div') : null),
    previewEffectsCanvas: (typeof document !== 'undefined' ? document.createElement('canvas') : null),
    contentBox: (typeof document !== 'undefined' ? document.createElement('div') : null),
    videoLayer: (typeof document !== 'undefined' ? document.createElement('div') : null),
    videoDimLayer: (typeof document !== 'undefined' ? document.createElement('div') : null),
    fadeLayer: (typeof document !== 'undefined' ? document.createElement('div') : null),
    // Trình phát (đồng hồ ảo) + waveform
    playerWrapper: document.querySelector('.player-wrapper'),
    playerControls: document.getElementById('player-controls'),
    virtualPlayBtn: document.getElementById('virtual-play-btn'),
    virtualTime: document.getElementById('virtual-time'),
    virtualSeek: document.getElementById('virtual-seek'),
    speedSelect: document.getElementById('speed-select'),
    waveformContainer: document.getElementById('waveform-container'),
    waveformInner: document.getElementById('waveform-inner'),
    waveformCanvas: document.getElementById('waveform-canvas'),
    waveformProgress: document.getElementById('waveform-progress'),
    // Editor JSON + Style drawer
    jsonEditor: document.getElementById('json-editor'),
    timeDisplay: document.getElementById('time-display'),
    fontListContainer: document.getElementById('font-list-container'),
    effectListContainer: document.getElementById('effect-list-container'),
    jsonSearchScope: document.getElementById('json-search-scope'),
    jsonSearchInput: document.getElementById('json-search-input'),
    jsonSearchPrevBtn: document.getElementById('json-search-prev-btn'),
    jsonSearchNextBtn: document.getElementById('json-search-next-btn'),
    jsonSearchCount: document.getElementById('json-search-count'),
    // Action Drawers
    aiLyricsBtn: document.getElementById('ai-lyrics-btn'),
    aiLyricsDrawer: document.getElementById('ai-lyrics-drawer'),
    exportDrawer: document.getElementById('export-drawer'),
    exportModal: document.getElementById('export-drawer'), // alias tương thích ngược
    mediaDrawer: document.getElementById('media-drawer'),
    styleDrawer: document.getElementById('style-drawer'),
    cancelExportBtn: document.getElementById('cancel-export-btn'),
    startExportBtn: document.getElementById('start-export-btn'),
    exportRes: document.getElementById('export-res'),
    exportFps: document.getElementById('export-fps'),
    exportBitrate: document.getElementById('export-bitrate'),
    exportCodec: document.getElementById('export-codec'),
    // Loading overlay + canvas ẩn dùng khi export
    loadingOverlay: document.getElementById('loading-overlay'),
    exportProgress: document.getElementById('export-progress'),
    exportCanvas: document.getElementById('export-canvas')
};
// Cho phép lăn chuột scroll ngang tự nhiên cho thanh README Tech Badges Bar
if (typeof window !== 'undefined') {
    window.addEventListener('DOMContentLoaded', () => {
        const readmeBar = document.querySelector('.readme-title-bar');
        if (readmeBar) {
            readmeBar.addEventListener('wheel', (e) => {
                if (e.deltaY !== 0) {
                    e.preventDefault();
                    readmeBar.scrollLeft += e.deltaY;
                }
            }, { passive: false });
        }
    });
}
// Thiết lập liên kết sự kiện cho Activity Bar, Master Header, Side Panel & Dashboard
if (typeof window !== 'undefined') {
    // 1. Quản lý Audio Catalog & Recommended Music động từ Asset/manifest.json
    let currentAudioPreview = null;
    let currentPlayingTrackRow = null;
    let audioCatalogData = null;

    function formatTimeSec(seconds) {
        if (!seconds || isNaN(seconds)) return '00:00';
        const m = Math.floor(seconds / 60);
        const s = Math.floor(seconds % 60);
        return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
    }

    function parseDurationSeconds(dur) {
        if (!dur) return 180;
        if (typeof dur === 'number') return dur;
        if (typeof dur === 'string') {
            const parts = dur.split(':').map(Number);
            if (parts.length === 2 && !isNaN(parts[0]) && !isNaN(parts[1])) {
                return parts[0] * 60 + parts[1];
            }
            if (parts.length === 3 && !isNaN(parts[0]) && !isNaN(parts[1]) && !isNaN(parts[2])) {
                return parts[0] * 3600 + parts[1] * 60 + parts[2];
            }
        }
        return 180;
    }

    function addAudioTrackToTimeline(track, elItem) {
        const curMs = Math.max(0, Math.round(window.getAccurateTimeMs ? window.getAccurateTimeMs() : (window.clock ? window.clock.currentTimeMs : 0)));
        const sec = parseDurationSeconds(track.duration);
        const durMs = sec * 1000;
        const trackTitle = track.name || track.title || 'Audio Track';
        const trackSrc = track.path || track.src || `/Asset/audio/${track.name || trackTitle}`;
        const newTrack = {
            type: 'audio',
            name: trackTitle,
            title: trackTitle,
            artist: track.artist || track.type || 'Music',
            start: curMs,
            end: curMs + durMs,
            duration: durMs,
            src: trackSrc
        };
        const rawTimeline = (typeof window.getProjectTabData === 'function')
            ? window.getProjectTabData('timeline')
            : (window.state && Array.isArray(window.state.timeline) ? window.state.timeline : []);
        const timelineList = Array.isArray(rawTimeline) ? [...rawTimeline] : [];
        let insertIdx = timelineList.length;
        for (let i = 0; i < timelineList.length; i++) {
            const it = timelineList[i];
            const s = (it && typeof it.start === 'number') ? it.start : 0;
            if (s > curMs) {
                insertIdx = i;
                break;
            }
        }
        timelineList.splice(insertIdx, 0, newTrack);
        if (typeof window.updateProjectTabData === 'function') {
            window.updateProjectTabData('timeline', timelineList, {
                highlightSnippet: `"src": "${trackSrc}"`,
                startMs: curMs,
                focusEditor: false
            });
        }
        if (elItem) {
            elItem.classList.add('added-flash');
            setTimeout(() => elItem.classList.remove('added-flash'), 400);
        }
        if (window.UI && UI.statusText) {
            UI.statusText.innerText = `✓ Đã thêm nhạc "${trackTitle}" vào timeline`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => UI.statusText.classList.remove('is-success'), 2000);
        }
    }

    function playAudioPreview(track, row) {
        const audioSrc = track.path || track.src || `/Asset/audio/${track.name}`;
        const streamUrl = (window.ResourceSystem ? window.ResourceSystem.resolveUrl(audioSrc) : (window.resolveFileUrl ? window.resolveFileUrl(audioSrc) : audioSrc)) || audioSrc;
        if (currentAudioPreview && currentPlayingTrackRow === row) {
            if (!currentAudioPreview.paused) {
                currentAudioPreview.pause();
                row.classList.remove('is-playing');
                resetCoverIcon(row);
                return;
            } else {
                currentAudioPreview.play();
                row.classList.add('is-playing');
                setPlayingCoverIcon(row);
                return;
            }
        }
        if (currentAudioPreview) {
            currentAudioPreview.pause();
            currentAudioPreview = null;
            if (currentPlayingTrackRow) {
                currentPlayingTrackRow.classList.remove('is-playing');
                resetCoverIcon(currentPlayingTrackRow);
            }
        }
        currentAudioPreview = new Audio(streamUrl);
        currentPlayingTrackRow = row;
        row.classList.add('is-playing');
        setPlayingCoverIcon(row);
        currentAudioPreview.play().catch(e => {
            console.warn('[AudioPreview] playback error:', e);
            row.classList.remove('is-playing');
            resetCoverIcon(row);
        });
        currentAudioPreview.onended = () => {
            row.classList.remove('is-playing');
            resetCoverIcon(row);
            currentAudioPreview = null;
            currentPlayingTrackRow = null;
        };
    }

    function setPlayingCoverIcon(row) {
        const cover = row.querySelector('.audio-track-cover');
        if (cover) {
            cover.innerHTML = `<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="4" width="4" height="16"></rect><rect x="14" y="4" width="4" height="16"></rect></svg>`;
            cover.style.background = 'rgba(16, 185, 129, 0.2)';
            cover.style.color = '#10b981';
        }
    }

    function resetCoverIcon(row) {
        const cover = row.querySelector('.audio-track-cover');
        if (cover) {
            cover.innerHTML = `<svg class="audio-note-icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#828294" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle></svg>`;
            cover.style.background = 'rgba(255, 255, 255, 0.08)';
            cover.style.color = '';
        }
    }

    async function fetchAudioCatalogData() {
        if (window.ResourceSystem) {
            await window.ResourceSystem.loadManifest('Asset/manifest.json');
            audioCatalogData = window.ResourceSystem.getAudioCatalog();
            return audioCatalogData;
        }
        if (audioCatalogData) return audioCatalogData;
        if (window.AudioCatalog && (window.AudioCatalog.Recommended || window.AudioCatalog.type)) {
            audioCatalogData = window.AudioCatalog;
            return audioCatalogData;
        }
        try {
            const resp = await fetch(new URL('Asset/manifest.json', document.baseURI).href, { cache: 'no-store' });
            if (resp.ok) {
                const json = await resp.json();
                if (json && json.audio) {
                    if (Array.isArray(json.audio) && json.audio[0]) {
                        audioCatalogData = json.audio[0];
                    } else if (typeof json.audio === 'object') {
                        audioCatalogData = json.audio;
                    }
                }
            }
        } catch (e) {
            console.warn('[AudioCatalog] fetch error:', e);
        }
        return audioCatalogData;
    }

    function createAudioTrackRow(track) {
        const row = document.createElement('div');
        row.className = 'audio-track-item';
        const genreStr = (track.type || track.genre || 'all').toLowerCase();
        row.dataset.genre = genreStr;
        row.dataset.title = (track.name || track.title || '').toLowerCase();
        row.dataset.artist = (track.artist || track.type || '').toLowerCase();
        const trackKey = track.path || track.src || track.name;
        row.dataset.trackPath = trackKey;
        row.title = `Click để thêm "${track.name}" vào video`;

        const durStr = track.duration || '00:00';
        const artistStr = track.artist || track.type || 'Audio';

        row.innerHTML = `
            <div class="audio-track-cover" title="Click để nghe thử">
                <svg class="audio-note-icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#828294" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M9 18V5l12-2v13"></path>
                    <circle cx="6" cy="18" r="3"></circle>
                    <circle cx="18" cy="16" r="3"></circle>
                </svg>
            </div>
            <div class="audio-track-meta">
                <span class="audio-track-title">${track.name}</span>
                <span class="audio-track-sub">${durStr} · ${artistStr}</span>
            </div>
            <div class="audio-track-actions">
                <button type="button" class="audio-track-btn add-btn" title="Thêm vào timeline">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"></line><line x1="5" y1="12" x2="19" y2="12"></line></svg>
                </button>
            </div>
        `;

        if (!track.duration || track.duration === '00:00') {
            const streamUrl = (window.ResourceSystem ? window.ResourceSystem.resolveUrl(track.path) : (window.resolveFileUrl ? window.resolveFileUrl(track.path) : track.path)) || track.path;
            const probe = new Audio(streamUrl);
            probe.addEventListener('loadedmetadata', () => {
                if (probe.duration && isFinite(probe.duration)) {
                    const sec = Math.round(probe.duration);
                    track.duration = formatTimeSec(sec);
                    const subEl = row.querySelector('.audio-track-sub');
                    if (subEl) subEl.textContent = `${track.duration} · ${artistStr}`;
                }
            });
        }

        const cover = row.querySelector('.audio-track-cover');
        if (cover) {
            cover.addEventListener('click', (e) => {
                e.stopPropagation();
                playAudioPreview(track, row);
            });
        }

        const addBtn = row.querySelector('.add-btn');
        if (addBtn) {
            addBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                addAudioTrackToTimeline(track, row);
            });
        }

        row.addEventListener('click', () => {
            addAudioTrackToTimeline(track, row);
        });

        return row;
    }

    async function renderRecommendedMusic() {
        const listEl = document.getElementById('media-audio-list');
        if (!listEl) return;

        const data = await fetchAudioCatalogData();
        const types = (data && Array.isArray(data.type)) ? data.type : ["All", "Phonk", "Background music", "Pop", "Chill", "R&B", "High Tempo"];
        const categories = (data && Array.isArray(data.Categories)) ? data.Categories : ["RECOMMEND", "POP", "R&B", "HEALING", "WARM", "HIGH TEMPO"];
        const recommended = (data && Array.isArray(data.Recommended)) ? data.Recommended : [];

        // 1. Render filter pills (audio-quick-chips) - RENDER ONCE
        const chipsContainer = document.querySelector('#audio-sub-music .audio-quick-chips');
        if (chipsContainer && chipsContainer.children.length === 0) {
            types.forEach((t, i) => {
                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = `audio-chip ${i === 0 ? 'active' : ''}`;
                btn.dataset.genre = t.toLowerCase();
                btn.textContent = t;
                btn.addEventListener('click', () => {
                    chipsContainer.querySelectorAll('.audio-chip').forEach(c => c.classList.remove('active'));
                    btn.classList.add('active');
                    filterAudioList(t.toLowerCase());
                });
                chipsContainer.appendChild(btn);
            });
        }

        // 2. Render categories grid (audio-categories-grid) - RENDER ONCE
        const catGrid = document.querySelector('#audio-sub-music .audio-categories-grid');
        if (catGrid && catGrid.children.length === 0) {
            categories.forEach(cat => {
                const card = document.createElement('div');
                card.className = 'audio-cat-card';
                const catTitle = typeof cat === 'string' ? cat : (cat.title || cat.name || '');
                card.innerHTML = `<span class="audio-cat-title">${catTitle}</span>`;
                card.title = `Lọc theo thể loại: ${catTitle}`;
                card.addEventListener('click', () => {
                    const targetText = catTitle.toLowerCase();
                    let matchedChip = null;
                    if (chipsContainer) {
                        chipsContainer.querySelectorAll('.audio-chip').forEach(c => {
                            const cText = c.textContent.trim().toLowerCase();
                            const cGenre = (c.dataset.genre || '').toLowerCase();
                            if (cText === targetText || cGenre === targetText || targetText.includes(cGenre) || cGenre.includes(targetText)) {
                                matchedChip = c;
                            }
                        });
                    }
                    if (matchedChip) {
                        matchedChip.click();
                    } else {
                        if (chipsContainer) {
                            chipsContainer.querySelectorAll('.audio-chip').forEach(c => c.classList.remove('active'));
                        }
                        filterAudioList(targetText);
                    }
                });
                catGrid.appendChild(card);
            });
        }

        // 3. Incremental DOM Reconciliation (NO full re-render / NO listEl.innerHTML = '')
        const existingRows = new Map();
        listEl.querySelectorAll('.audio-track-item').forEach(row => {
            const key = row.dataset.trackPath || row.dataset.title;
            if (key) existingRows.set(key, row);
        });

        const activeTrackKeys = new Set();
        recommended.forEach(track => {
            const trackKey = track.path || track.src || track.name;
            activeTrackKeys.add(trackKey);

            if (existingRows.has(trackKey)) {
                return; // Giữ nguyên node DOM hiện có
            }

            const row = createAudioTrackRow(track);
            listEl.appendChild(row);
        });

        // Xóa các track không còn trong danh sách
        for (const [key, row] of existingRows.entries()) {
            if (!activeTrackKeys.has(key)) {
                row.remove();
            }
        }

        // 4. Lắng nghe tìm kiếm nhạc (Search input)
        const searchInput = document.getElementById('audio-search-input');
        if (searchInput && !searchInput._bound) {
            searchInput._bound = true;
            searchInput.addEventListener('input', () => {
                const activeChip = document.querySelector('#audio-sub-music .audio-quick-chips .audio-chip.active');
                const genre = activeChip ? activeChip.dataset.genre : 'all';
                filterAudioList(genre);
            });
        }
    }

    if (window.ResourceSystem) {
        window.ResourceSystem.on('add', (desc) => {
            if (desc && desc.type === 'audio') {
                const listEl = document.getElementById('media-audio-list');
                if (listEl) {
                    const trackKey = desc.path || desc.name;
                    const existing = listEl.querySelector(`[data-track-path="${CSS.escape(trackKey)}"]`);
                    if (!existing) {
                        listEl.appendChild(createAudioTrackRow(desc));
                    }
                }
            }
        });
        window.ResourceSystem.on('remove', (desc) => {
            if (desc && desc.type === 'audio') {
                const listEl = document.getElementById('media-audio-list');
                if (listEl) {
                    const trackKey = desc.path || desc.name;
                    const existing = listEl.querySelector(`[data-track-path="${CSS.escape(trackKey)}"]`);
                    if (existing) existing.remove();
                }
            }
        });
    }

    function filterAudioList(genreFilter) {
        const listEl = document.getElementById('media-audio-list');
        if (!listEl) return;
        const searchInput = document.getElementById('audio-search-input');
        const q = searchInput ? searchInput.value.trim().toLowerCase() : '';
        const filter = (genreFilter || 'all').toLowerCase();

        listEl.querySelectorAll('.audio-track-item').forEach(it => {
            const itemGenre = it.dataset.genre || '';
            const itemTitle = it.dataset.title || '';
            const itemArtist = it.dataset.artist || '';

            const genreMatch = (filter === 'all') || 
                (itemGenre === filter) || 
                itemGenre.includes(filter) || 
                itemArtist.includes(filter) ||
                (filter === 'recommend' || filter === 'recommended');

            const searchMatch = !q || 
                itemTitle.includes(q) || 
                itemArtist.includes(q) || 
                itemGenre.includes(q);

            it.style.display = (genreMatch && searchMatch) ? 'grid' : 'none';
        });
    }

    const BODY_EFFECTS_ITEMS = [
        { id: "body-glow", name: "Body Glow", tag: "Light", desc: "Phát sáng viền cơ thể mềm mại" },
        { id: "cyber-aura", name: "Cyber Aura", tag: "Sci-Fi", desc: "Vòng năng lượng lượng tử xung quanh người" },
        { id: "neon-outline", name: "Neon Outline", tag: "Line", desc: "Viền nét neon dạ quang chuyển động" },
        { id: "shadow-clone", name: "Shadow Clone", tag: "Motion", desc: "Bóng ma phân thân chuyển động trễ" },
        { id: "electro-spark", name: "Electro Spark", tag: "Energy", desc: "Tia sét điện quang nổ quanh nhân vật" },
        { id: "freeze-glaze", name: "Freeze Glaze", tag: "Ice", desc: "Hiệu ứng đóng băng cơ thể nghệ thuật" },
        { id: "particle-dust", name: "Particle Dust", tag: "Particles", desc: "Bụi vàng ma thuật tỏa ra từ chuyển động" },
        { id: "ghost-trail", name: "Ghost Trail", tag: "Echo", desc: "Vệt dư ảnh ảo mộng theo bước đi" }
    ];

    function renderBodyEffects() {
        const container = document.getElementById('body-effect-list-container');
        if (!container || container.children.length > 0) return;
        BODY_EFFECTS_ITEMS.forEach(eff => {
            const card = document.createElement('div');
            card.className = 'effect-card';
            card.title = `Click để thêm hiệu ứng ${eff.name}`;
            card.innerHTML = `
                <div class="effect-card-thumb" style="background: linear-gradient(135deg, #181822, #2a2a38); display:flex; align-items:center; justify-content:center; border-radius:4px; height:68px;">
                    <span style="font-size:0.75rem; color:var(--text-accent, #43ad6a); font-weight:600; text-transform:uppercase;">${eff.tag}</span>
                </div>
                <div class="effect-card-title">${eff.name}</div>
            `;
            card.addEventListener('click', () => {
                if (typeof addEffectToTimeline === 'function') {
                    addEffectToTimeline({
                        name: eff.name,
                        create: (s, e) => ({ type: 'effect', effect: eff.id, start: s, end: e })
                    }, card);
                }
            });
            container.appendChild(card);
        });
        const searchInput = document.getElementById('body-effect-search-input');
        if (searchInput && !searchInput._bound) {
            searchInput._bound = true;
            searchInput.addEventListener('input', () => {
                const q = searchInput.value.trim().toLowerCase();
                container.querySelectorAll('.effect-card').forEach(it => {
                    const txt = it.textContent.toLowerCase();
                    it.style.display = (!q || txt.includes(q)) ? 'grid' : 'none';
                });
            });
        }
    }
    function showCol2SubPane(targetId, groupClass) {
        document.querySelectorAll(`.${groupClass}`).forEach(p => {
            const isMatch = (p.id === targetId);
            p.classList.toggle('active', isMatch);
            p.style.display = isMatch ? '' : 'none';
        });
    }
    // 1. Chuyển đổi giữa các pane của Side Panel (Cột 2) & Header Cột 2 tương ứng
    const switchSidePane = (paneId, activeBtnId) => {
        // 1.1 Chuyển pane nội dung
        const panes = document.querySelectorAll('.side-pane');
        panes.forEach(p => {
            const isMatch = (p.id === paneId);
            p.style.display = isMatch ? '' : 'none';
            p.classList.toggle('active', isMatch);
        });
        // 1.2 Chuyển header Cột 2 tương ứng
        const hdrMap = {
            'pane-media': 'col2-hdr-media',
            'pane-agent': 'col2-hdr-agent',
            'pane-audio': 'col2-hdr-audio',
            'pane-text': 'col2-hdr-text',
            'pane-effect': 'col2-hdr-effect',
            'pane-debug': 'col2-hdr-debug'
        };
        const targetHdrId = hdrMap[paneId] || 'col2-hdr-media';
        const hdrPanes = document.querySelectorAll('.col2-hdr-pane');
        hdrPanes.forEach(h => {
            const isMatch = (h.id === targetHdrId);
            h.style.display = isMatch ? '' : 'none';
            h.classList.toggle('active', isMatch);
        });
        const actBtns = document.querySelectorAll('.activity-btn');
        actBtns.forEach(b => {
            b.classList.toggle('active', b.id === activeBtnId);
        });
        // 1.3 Đảm bảo sub-pane mặc định hiển thị nếu chưa có sub-pane nào active
        const activePane = document.getElementById(paneId);
        if (activePane) {
            const subPanes = activePane.querySelectorAll('.col2-sub-pane, .media-sub-pane, .audio-sub-pane, .text-sub-pane, .effect-sub-pane, .debug-sub-pane');
            const hasActiveSub = Array.from(subPanes).some(sp => sp.classList.contains('active') && sp.style.display !== 'none');
            if (!hasActiveSub && subPanes.length > 0) {
                subPanes.forEach((sp, idx) => {
                    const isFirst = (idx === 0);
                    sp.classList.toggle('active', isFirst);
                    sp.style.display = isFirst ? '' : 'none';
                });
                const targetHdr = document.getElementById(targetHdrId);
                if (targetHdr) {
                    const tabs = targetHdr.querySelectorAll('.col2-hdr-tab');
                    tabs.forEach((t, idx) => t.classList.toggle('active', idx === 0));
                }
            }
        }
        // Kích hoạt render dữ liệu tương ứng
        if (paneId === 'pane-media') {
            if (window.MediaDrawer && typeof window.MediaDrawer.switchTab === 'function') {
                window.MediaDrawer.switchTab('video', true);
            }
            if (window.MediaDrawer && typeof window.MediaDrawer.renderVideoBatch === 'function') {
                window.MediaDrawer.renderVideoBatch();
            }
        } else if (paneId === 'pane-audio') {
            renderRecommendedMusic();
            if (window.MediaDrawer && typeof window.MediaDrawer.renderSfxList === 'function') {
                window.MediaDrawer.renderSfxList();
            }
        } else if (paneId === 'pane-text') {
            if (typeof window.renderFontArtGrids === 'function') {
                window.renderFontArtGrids();
            }
            if (typeof window.ensureFontListLoaded === 'function') {
                window.ensureFontListLoaded();
            }
        } else if (paneId === 'pane-effect') {
            if (typeof renderAllCardThumbnails === 'function') {
                renderAllCardThumbnails();
            }
            renderBodyEffects();
        }
    };
    // 1.4 Thiết lập tabs bên trong Header Cột 2 (Music/Sound effects, Video/Body effects...)
    const setupCol2HeaderTabs = () => {
        const col2TabBtns = document.querySelectorAll('.col2-hdr-tab, .col2-tab-btn');
        col2TabBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                const parentNav = btn.closest('.col2-hdr-nav, .col2-tabs-nav');
                if (parentNav) {
                    parentNav.querySelectorAll('.col2-hdr-tab, .col2-tab-btn').forEach(b => b.classList.remove('active'));
                    btn.classList.add('active');
                }
                const tabKey = btn.dataset.col2Tab;
                // 1. Chuyển sub-tab Audio (Music | Sound effects)
                if (tabKey === 'audio-music') {
                    showCol2SubPane('audio-sub-music', 'audio-sub-pane');
                    renderRecommendedMusic();
                } else if (tabKey === 'audio-sfx') {
                    showCol2SubPane('audio-sub-sfx', 'audio-sub-pane');
                    if (window.MediaDrawer && typeof window.MediaDrawer.renderSfxList === 'function') {
                        window.MediaDrawer.renderSfxList();
                    }
                }
                // 2. Chuyển sub-tab Text (Templates | Font Family)
                else if (tabKey === 'text-templates') {
                    showCol2SubPane('text-sub-templates', 'text-sub-pane');
                    if (typeof window.renderFontArtGrids === 'function') {
                        window.renderFontArtGrids();
                    }
                } else if (tabKey === 'text-fonts') {
                    showCol2SubPane('text-sub-fonts', 'text-sub-pane');
                    if (typeof window.ensureFontListLoaded === 'function') {
                        window.ensureFontListLoaded();
                    }
                }
                // 3. Chuyển sub-tab Effect (Video effects | Body effects)
                else if (tabKey === 'effect-video') {
                    showCol2SubPane('effect-sub-video', 'effect-sub-pane');
                    if (typeof renderAllCardThumbnails === 'function') {
                        renderAllCardThumbnails();
                    }
                } else if (tabKey === 'effect-body') {
                    showCol2SubPane('effect-sub-body', 'effect-sub-pane');
                    renderBodyEffects();
                }
                // 4. Chuyển sub-tab Debug (Telemetry | Cache)
                else if (tabKey === 'debug-telemetry') {
                    showCol2SubPane('debug-sub-telemetry', 'debug-sub-pane');
                } else if (tabKey === 'debug-cache') {
                    showCol2SubPane('debug-sub-cache', 'debug-sub-pane');
                }
            });
        });
    };
    // 2. Thiết lập điều hướng Dashboard (Trang chính) & Studio Editor
    const setupDashboardNavigation = () => {
        const logoBtn = document.getElementById('activity-logo-btn');
        const dashView = document.getElementById('dashboard-view');
        const studioView = document.getElementById('studio-view');
        const projectNameEl = document.getElementById('dash-project-name');
        const showDashboard = () => {
            if (dashView && studioView) {
                studioView.style.display = 'none';
                dashView.style.display = 'grid';
                if (projectNameEl && window.state && window.state.sessionProjectName) {
                    projectNameEl.textContent = window.state.sessionProjectName;
                }
            }
        };
        const showStudio = () => {
            if (dashView && studioView) {
                dashView.style.display = 'none';
                studioView.style.display = 'grid';
                if (typeof initViewportSize === 'function') initViewportSize();
                if (typeof updateFrame === 'function') updateFrame();
            }
        };
        if (logoBtn) logoBtn.addEventListener('click', showDashboard);
        const projectCard = document.getElementById('dash-project-card');
        if (projectCard) projectCard.addEventListener('click', showStudio);
        const createNewBtn = document.getElementById('dash-create-new-btn');
        if (createNewBtn) createNewBtn.addEventListener('click', showStudio);
        const createVideoBtn = document.getElementById('dash-create-video-btn');
        if (createVideoBtn) createVideoBtn.addEventListener('click', showStudio);
        const uploadMediaBtn = document.getElementById('dash-upload-media-btn');
        const openFolderBtn = document.getElementById('dash-open-folder-btn');
        const fileInput = document.getElementById('fallback-folder-input');
        if (uploadMediaBtn && fileInput) {
            uploadMediaBtn.addEventListener('click', () => fileInput.click());
        }
        if (openFolderBtn && fileInput) {
            openFolderBtn.addEventListener('click', () => fileInput.click());
        }
    };
    // Dictionary SVG chu?n cho t?ng t? l? khung h?nh th?c t? (??ng b? t? l? th?t & n?t d?y d?n 1.8)
    const RATIO_SVGS = {
        'original': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="2.5" y="5" width="19" height="14" stroke-dasharray="3 2"></rect></svg>`,
        '16:9': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="2" y="6.375" width="20" height="11.25"></rect></svg>`,
        '4:3': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="5.25" width="18" height="13.5"></rect></svg>`,
        '2:1': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="2" y="7" width="20" height="10"></rect></svg>`,
        '9:16': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="6.375" y="2" width="11.25" height="20"></rect></svg>`,
        '1:1': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="4.5" y="4.5" width="15" height="15"></rect></svg>`,
        '3:4': `<svg class="ratio-icon" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="5.25" y="3" width="13.5" height="18"></rect></svg>`
    };
    function updateAspectRatioBtn(ratio) {
        const btnIcon = document.getElementById('ratio-btn-icon');
        if (btnIcon && RATIO_SVGS[ratio]) {
            btnIcon.innerHTML = RATIO_SVGS[ratio];
        }
        const currentText = document.getElementById('ratio-current-text');
        if (currentText) {
            currentText.textContent = (ratio === 'original') ? 'Orig' : ratio;
        }
    }
    if (typeof window !== 'undefined') {
        window.updateAspectRatioBtn = updateAspectRatioBtn;
    }
    // 3. Thi?t l?p Widget ??i T? L? Khung H?nh (Aspect Ratio Widget)
    const setupAspectRatioWidget = () => {
        const ratioBtn = document.getElementById('aspect-ratio-btn');
        const ratioPanel = document.getElementById('aspect-ratio-panel');
        if (!ratioBtn || !ratioPanel) return;
        ratioBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            ratioPanel.classList.toggle('is-open');
        });

        // Chỉ hiển thị widget khi chuột hover vào vùng panel/stage chứa viewport
        const widget = document.getElementById('aspect-ratio-widget');
        const stage = document.getElementById('fullscreen-stage');
        if (widget && stage) {
            let hideTimer = null;
            const onEnter = () => {
                if (hideTimer) { clearTimeout(hideTimer); hideTimer = null; }
                widget.classList.add('is-hovered');
            };
            const onLeave = () => {
                if (hideTimer) clearTimeout(hideTimer);
                hideTimer = setTimeout(() => {
                    if (!ratioPanel.classList.contains('is-open') && !widget.matches(':hover') && !stage.matches(':hover')) {
                        widget.classList.remove('is-hovered');
                    }
                }, 180);
            };

            stage.addEventListener('mouseenter', onEnter);
            stage.addEventListener('mouseleave', onLeave);
            widget.addEventListener('mouseenter', onEnter);
            widget.addEventListener('mouseleave', onLeave);
        }
        document.addEventListener('click', (e) => {
            if (!e.target.closest('#aspect-ratio-widget')) {
                ratioPanel.classList.remove('is-open');
            }
        });
        const options = ratioPanel.querySelectorAll('.ratio-option');
        // Kh?i t?o tr?ng th?i ban ??u: m?c ??nh lu?n l? 'original' theo quy chu?n
        const initialRatio = (window.state && window.state.config && window.state.config.aspectRatio) ? window.state.config.aspectRatio : 'original';
        updateAspectRatioBtn(initialRatio);
        options.forEach(o => {
            o.classList.toggle('active', o.dataset.ratio === initialRatio);
        });
        options.forEach(opt => {
            opt.addEventListener('click', async (e) => {
                e.stopPropagation();
                const ratio = opt.dataset.ratio;
                options.forEach(o => o.classList.remove('active'));
                opt.classList.add('active');
                updateAspectRatioBtn(ratio);
                ratioPanel.classList.remove('is-open');
                if (window.state) {
                    if (!window.state.config) window.state.config = {};
                    window.state.config.aspectRatio = ratio;
                    delete window.state.config.baseW;
                    delete window.state.config.baseH;
                    delete window.state.config.width;
                    delete window.state.config.height;
                    if (typeof fullProjectData !== 'undefined' && fullProjectData && fullProjectData.config) {
                        fullProjectData.config.aspectRatio = ratio;
                        delete fullProjectData.config.baseW;
                        delete fullProjectData.config.baseH;
                        window.state.ram.projectJson = JSON.stringify(fullProjectData, null, " ");
                    }
                    // 1. T?nh to?n l?i baseW, baseH v? renderConfig
                    if (typeof buildRenderConfig === 'function') {
                        await buildRenderConfig();
                    }
                    // 2. ??y thay ??i l?p t?c v?o C++ Engine Core qua doJsonMove / applyJsonMove (kh?ng ph?i b?m cho vui)
                    const stageW = (window.state.renderConfig && window.state.renderConfig.baseW) || 1920;
                    const stageH = (window.state.renderConfig && window.state.renderConfig.baseH) || 1080;
                    const currentJson = (window.state.ram && window.state.ram.projectJson) || JSON.stringify(window.state);
                    if (window.TimelineEngine && typeof window.TimelineEngine.applyJsonMove === 'function') {
                        try {
                            window.TimelineEngine.applyJsonMove(fullProjectData || window.state, currentJson);
                        } catch (err) {
                            console.warn('[Aspect Ratio] applyJsonMove warning:', err);
                        }
                    } else if (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.manager && window.WasmBridge.manager.doJsonMove) {
                        try {
                            window.WasmBridge.manager.doJsonMove(currentJson, stageW, stageH);
                        } catch (err) {
                            console.warn('[Aspect Ratio] WasmBridge doJsonMove warning:', err);
                        }
                    }
                    // 3. Khởi tạo lại kích thước viewport và render khung hình mới
                    if (typeof initViewportSize === 'function') {
                        initViewportSize();
                    }
                    if (typeof window.syncViewportToConfig === 'function') {
                        window.syncViewportToConfig();
                    }
                    if (typeof updateFrame === 'function') {
                        const curTime = (typeof getAccurateTimeMs === 'function') ? getAccurateTimeMs() : (window.state.playheadMs || 0);
                        updateFrame(curTime);
                    }
                    // 4. Đồng bộ text trong json editor nếu tab config đang mở
                    if (typeof currentJsonTab !== 'undefined' && currentJsonTab === 'config' && window.UI && UI.jsonEditor) {
                        UI.jsonEditor.value = JSON.stringify(window.state.config, null, " ");
                    }
                }
            });
        });
    };
    // 4. Thiết lập Sub-tabs trong Pane Media ([Video, Image]) & Ô tìm kiếm Media
    const setupMediaSubTabs = () => {
        const subBtns = document.querySelectorAll('.media-sub-btn');
        subBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                const sub = btn.dataset.sub;
                subBtns.forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                const vidPane = document.getElementById('media-sub-video');
                const imgPane = document.getElementById('media-sub-image');
                if (vidPane) {
                    vidPane.classList.toggle('active', sub === 'video');
                    vidPane.style.display = (sub === 'video') ? 'grid' : 'none';
                }
                if (imgPane) {
                    imgPane.classList.toggle('active', sub === 'image');
                    imgPane.style.display = (sub === 'image') ? 'grid' : 'none';
                }
                if (window.MediaDrawer && typeof window.MediaDrawer.switchTab === 'function') {
                    window.MediaDrawer.switchTab(sub);
                }
            });
        });
        // Tìm kiếm nhanh trong tab Media
        const mediaSearch = document.getElementById('media-search-input');
        if (mediaSearch) {
            mediaSearch.addEventListener('input', (e) => {
                const q = e.target.value.trim().toLowerCase();
                const cards = document.querySelectorAll('#pane-media .media-card');
                cards.forEach(c => {
                    const title = (c.querySelector('.media-card-name, .media-card-title')?.textContent || '').toLowerCase();
                    c.style.display = (!q || title.includes(q)) ? '' : 'none';
                });
            });
        }
    };
    // 5. Kết nối toàn bộ sự kiện cho Activity Bar & Header
    const setupActivityBar = () => {
        setupDashboardNavigation();
        setupAspectRatioWidget();
        setupMediaSubTabs();
        setupCol2HeaderTabs();
        const actMedia = document.getElementById('act-media-btn');
        if (actMedia) {
            actMedia.addEventListener('click', () => {
                switchSidePane('pane-media', 'act-media-btn');
            });
        }
        const actAgent = document.getElementById('act-agent-btn');
        if (actAgent) {
            actAgent.addEventListener('click', () => {
                switchSidePane('pane-agent', 'act-agent-btn');
            });
        }
        const actAudio = document.getElementById('act-audio-btn');
        if (actAudio) {
            actAudio.addEventListener('click', () => {
                switchSidePane('pane-audio', 'act-audio-btn');
            });
        }
        const actText = document.getElementById('act-text-btn');
        if (actText) {
            actText.addEventListener('click', () => {
                switchSidePane('pane-text', 'act-text-btn');
            });
        }
        const actEffect = document.getElementById('act-effect-btn');
        if (actEffect) {
            actEffect.addEventListener('click', () => {
                switchSidePane('pane-effect', 'act-effect-btn');
            });
        }
        const actDebug = document.getElementById('act-debug-btn');
        if (actDebug) {
            actDebug.addEventListener('click', () => {
                switchSidePane('pane-debug', 'act-debug-btn');
            });
        }
        const actSettings = document.getElementById('act-settings-btn');
        if (actSettings) {
            actSettings.addEventListener('click', () => {
                switchSidePane('pane-debug', 'act-settings-btn');
            });
        }
        const headerMoreBtn = document.getElementById('header-more-btn');
        if (headerMoreBtn) {
            headerMoreBtn.addEventListener('click', () => {
                switchSidePane('pane-debug', 'act-debug-btn');
            });
        }
        const headerProjectPill = document.getElementById('header-project-pill');
        if (headerProjectPill && UI.loadBtn) {
            headerProjectPill.addEventListener('click', () => UI.loadBtn.click());
        }
    };
    if (document.readyState === 'loading') {
        window.addEventListener('DOMContentLoaded', setupActivityBar);
    } else {
        setupActivityBar();
    }
}
