// ============================================================================
// TABS — chuyển tab Font / Effect ở panel trái & quản lý danh sách Effect
//   Phụ thuộc : ui.js, state.js, clock.js, timeline-engine.js
//   Cung cấp  : addEffectToTimeline, AVAILABLE_EFFECTS
// ============================================================================
const tabBtns = document.querySelectorAll('.tab-btn[data-target]');
const tabContents = document.querySelectorAll('.tab-content');
tabBtns.forEach(btn => {
    btn.addEventListener('click', () => {
        if (!btn.dataset.target) return;
        const targetEl = document.getElementById(btn.dataset.target);
        if (!targetEl) return;
        tabBtns.forEach(b => b.classList.remove('active'));
        tabContents.forEach(c => c.classList.remove('active'));
        btn.classList.add('active');
        tabBtns.forEach(b => b.setAttribute('aria-selected', String(b === btn)));
        targetEl.classList.add('active');
        const fontSearch = document.getElementById('font-search-input');
        const effectSearch = document.getElementById('effect-search-input');
        if (btn.dataset.target === 'tab-fonts') {
            if (fontSearch) fontSearch.style.display = '';
            if (effectSearch) effectSearch.style.display = 'none';
        } else if (btn.dataset.target === 'tab-effects') {
            if (fontSearch) fontSearch.style.display = 'none';
            if (effectSearch) effectSearch.style.display = '';
            renderAllCardThumbnails();
        }
    });
});
// Danh sách toàn bộ các hiệu ứng engine hỗ trợ
const AVAILABLE_EFFECTS = [
    {
        id: 'zoom',
        name: 'Zoom',
        tag: 'Frame',
        desc: 'Phóng to thu nhỏ tuần hoàn theo nhịp',
        create: (start, end) => ({
            type: 'effect',
            effect: 'zoom',
            start,
            end,
            amplitude: 0.08,
            speed: 1
        })
    },
    {
        id: 'zoom-bass',
        name: 'Zoom',
        tag: 'Sound',
        desc: 'Phóng khung hình đập theo nhịp bass',
        create: (start, end) => ({
            type: 'effect',
            effect: 'zoom',
            source: 'bass',
            start,
            end,
            amplitude: 0.06
        })
    },
    {
        id: 'shake',
        name: 'Shake',
        tag: 'Frame',
        desc: 'Rung lắc toàn màn hình, không lộ viền',
        create: (start, end) => ({
            type: 'effect',
            effect: 'shake',
            start,
            end,
            speed: 12,
            amplitude: 0.008,
            tilt: 0.5,
            fade: 200
        })
    },
    {
        id: 'glitch',
        name: 'Glitch',
        tag: 'Particle',
        desc: 'Cắt sọc ngang dịch trái & khối copy-paste',
        create: (start, end) => ({
            type: 'effect',
            effect: 'glitch',
            start,
            end,
            speed: 2
        })
    },
    {
        id: 'matrix',
        name: 'Matrix',
        tag: 'Frame',
        desc: 'Số nhòe, chuỗi ký tự pixel & vạch quét',
        create: (start, end) => ({
            type: 'effect',
            effect: 'matrix',
            start,
            end,
            color: '#f0e14a',
            speed: 1,
            density: 1,
            dim: 0,
            fade: 150
        })
    },
    {
        id: 'rain-drop',
        name: 'Rain Drop',
        tag: 'Water',
        desc: 'Hiệu ứng giọt nước mưa trên kính toàn phần 5 tầng (NodeVideo _RaindropPro: static1, static2, dynamic, rain-trail, trail2)',
        create: (start, end) => ({
            type: 'effect',
            effect: 'rain drop',
            start,
            end,
            strength: 1,
            blur_type: 'glass',
            blur: 0,
            childs: [
                {
                    'rain-static-1': false,
                    amount: 0.5,
                    intensity: 1.0,
                    scale: 4.0,
                    noise: 1.2,
                    speed: 0.2
                },
                {
                    'rain-static-2': false,
                    amount: 0.5,
                    intensity: 1.0,
                    scale: 1.5,
                    noise: 1.2,
                    speed: 0.1
                },
                {
                    'rain-dynamic': false,
                    amount: 0.6,
                    intensity: 1.0,
                    scale: 2.0,
                    speed: 0.2
                },
                {
                    'rain-trail': false,
                    seed: 0,
                    speed: 1,
                    amount: 30,
                    intensity: 0.8,
                    scale: 1,
                    start_size: [0.3, 0.5],
                    _spread: 0.45,
                    _start_lifetime: [1, 10],
                    _start_speed: [0, 5],
                    _direction: 0,
                    _radial: 0.2,
                    _offset: 0,
                    _noise: 0.2,
                    _prewarm: true
                },
                {
                    'rain-trail-2': false,
                    _strength2: 0.3,
                    _amount4: 25.0,
                    _speed4: [5.0, 15.0],
                    _scale4: [2.0, 5.0],
                    _intensity4: [0.6, 1.0],
                    _rotation: 0.0,
                    _random_rotation: 0.1,
                    _random_pos: 1.0,
                    _tint: '#000000',
                    _seed2: 0
                }
            ],
            fade: 200
        })
    },
    {
        id: 'turbulent',
        name: 'Turbulent',
        tag: 'Distort',
        desc: 'Dòng chảy hỗn loạn bẻ cong bề mặt hữu cơ',
        create: (start, end) => ({
            type: 'effect',
            effect: 'turbulent',
            start,
            end,
            strength: 1,
            speed: 1,
            frequency: 1,
            octaves: 3,
            fade: 300
        })
    },
    {
        id: 'lens',
        name: 'Lens Distort',
        tag: 'Distort',
        desc: 'Thấu kính quang học mắt cá Fisheye & viền tán sắc',
        create: (start, end) => ({
            type: 'effect',
            effect: 'lens',
            start,
            end,
            curvature: 0.5,
            scale: 1,
            dispersion: 0.02,
            fade: 300
        })
    },
    {
        id: 'twirl',
        name: 'Twirl',
        tag: 'Distort',
        desc: 'Xoáy vặn lốc xoáy quanh tâm màn hình',
        create: (start, end) => ({
            type: 'effect',
            effect: 'twirl',
            start,
            end,
            angle: 3.14,
            radius: 0.5,
            speed: 0.5,
            fade: 300
        })
    },
    {
        id: 'chromatic',
        name: 'Chromatic',
        tag: 'Color',
        desc: 'Phân tách 3 kênh màu RGB tán sắc lăng kính',
        create: (start, end) => ({
            type: 'effect',
            effect: 'chromatic',
            start,
            end,
            distance: 1,
            angle: 0,
            radial: 0,
            fade: 300
        })
    },
    {
        id: 'pinch',
        name: 'Pinch/Bulge',
        tag: 'Distort',
        desc: 'Phình to kính lúp hoặc hút lõm hố đen',
        create: (start, end) => ({
            type: 'effect',
            effect: 'pinch',
            start,
            end,
            amount: 0.5,
            radius: 0.45,
            fade: 300
        })
    },
    {
        id: 'zoom-blur',
        name: 'Zoom Blur',
        tag: 'Frame',
        desc: 'Phóng mờ hướng tâm đa mẫu lượn sóng',
        create: (start, end) => ({
            type: 'effect',
            effect: 'zoom-blur',
            start,
            end,
            strength: 0.4,
            centerX: 0.5,
            centerY: 0.5,
            fade: 250
        })
    },
    {
        id: 'motion-blur',
        name: 'Motion Blur',
        tag: 'Frame',
        desc: 'Làm nhòe vệt chuyển động hướng tốc',
        create: (start, end) => ({
            type: 'effect',
            effect: 'motion-blur',
            start,
            end,
            strength: 1,
            angle: 0,
            fade: 200
        })
    },
    {
        id: 'glitch-rgb',
        name: 'RGB Prism',
        tag: 'Particle',
        desc: 'Glitch tán sắc lăng kính quang phổ đa tầng',
        create: (start, end) => ({
            type: 'effect',
            effect: 'glitch-rgb',
            start,
            end,
            amount: 0.5,
            speed: 1,
            dispersion: 1,
            fade: 100
        })
    },
    {
        id: 'cyber-scan',
        name: 'Cyber Scan',
        tag: 'Frame',
        desc: 'Quét vệt sóng biến dạng & bóng đổ nếp gấp',
        create: (start, end) => ({
            type: 'effect',
            effect: 'cyber-scan',
            start,
            end,
            intensity: 1,
            speed: 1,
            scanStrength: 1,
            fade: 200
        })
    },
    {
        id: 'fire',
        name: 'Fire Flame',
        tag: 'Light',
        desc: 'Lửa bốc cháy hữu cơ FBM hoặc viền lửa 4 cạnh',
        create: (start, end) => ({
            type: 'effect',
            effect: 'fire',
            start,
            end,
            mode: 'full',
            intensity: 1,
            speed: 1,
            scale: 1,
            fade: 300
        })
    },
    {
        id: 'rain-bokeh',
        name: 'Drive Home',
        tag: 'Light',
        desc: 'Mưa đêm kính xe & đèn bokeh giao thông 3D',
        create: (start, end) => ({
            type: 'effect',
            effect: 'rain-bokeh',
            start,
            end,
            intensity: 1,
            speed: 1,
            rainStrength: 1,
            camShake: 0.8,
            blend: 'screen',
            fade: 400
        })
    },
    {
        id: 'bloom',
        name: 'Bloom HDR',
        tag: 'Light',
        desc: 'Glow tỏa sáng siêu mịn 6-LOD Bilinear ReinhardExtLuma theo màu hoặc độ sáng',
        create: (start, end) => ({
            type: 'effect',
            effect: 'bloom',
            start,
            end,
            color: '#ffffff',
            tolerance: 0.4,
            intensity: 1.0,
            threshold: 0.2,
            radius: 1.5,
            bloomFactor: 0.8,
            fade: 200
        })
    },
    {
        id: 'film-burn',
        name: 'Film Burn',
        tag: 'Frame',
        desc: 'Cháy màng phim than hồng tan chảy chuyển cảnh',
        create: (start, end) => ({
            type: 'effect',
            effect: 'film-burn',
            start,
            end,
            speed: 1,
            emberGlow: 1.5,
            distortion: 1,
            fade: 200
        })
    },
    {
        id: 'fxaa',
        name: 'FXAA',
        tag: 'Frame',
        desc: 'Khử răng cưa toàn diện NVIDIA FXAA 3.11 mịn mượt chữ và hình',
        create: (start, end) => ({
            type: 'effect',
            effect: 'fxaa',
            start,
            end,
            subpix: 0.75,
            edgeThreshold: 0.166,
            edgeThresholdMin: 0.0833,
            fade: 150
        })
    },
    {
        id: 'flicker',
        name: 'Flicker',
        tag: 'Light',
        desc: 'Nhấp nháy sáng tối đều đặn tuần hoàn',
        create: (start, end) => ({
            type: 'effect',
            effect: 'flicker',
            start,
            end,
            speed: 2,
            brightness: 0.5,
            shape: 'sine'
        })
    },
    {
        id: 'flicker-bass',
        name: 'Flicker',
        tag: 'Sound',
        desc: 'Nhấp nháy sáng bùng theo nhịp bass',
        create: (start, end) => ({
            type: 'effect',
            effect: 'flicker',
            source: 'bass',
            start,
            end,
            brightness: 0.6,
            cutoff: 150
        })
    },
    {
        id: 'bg-bass',
        name: 'Background',
        tag: 'Sound',
        desc: 'Đảo màu nền & chữ theo từng nhịp bass tuần hoàn',
        create: (start, end) => ({
            type: 'background',
            source: 'bass',
            start,
            end: start + 15000,
            colors: [
                { "background-color": "#000", "color": "#fff" },
                { "background-color": "#fff", "color": "#000" },
            ]
        })
    },
    {
        id: 'intro',
        name: 'Intro',
        tag: 'Switch',
        desc: 'Hố đen, dải ngang răng lược & vòng sáng',
        create: (start, end) => ({
            type: 'effect',
            effect: 'intro',
            start,
            end
        })
    },
    {
        id: 'outro',
        name: 'Outro',
        tag: 'Switch',
        desc: 'Thu hẹp hố đen, dải ngang & chấm sáng',
        create: (start, end) => ({
            type: 'effect',
            effect: 'outro',
            start,
            end
        })
    }
];
// Preload hoannaoh.png cho ảnh nền preview 1s của thẻ hiệu ứng
const previewBg = new Image();
let isPreviewBgLoaded = false;
previewBg.onload = () => {
    isPreviewBgLoaded = true;
    renderAllCardThumbnails();
};
previewBg.src = 'Asset/image/hoannaoh.png';
const PREVIEW_W = 160;
const PREVIEW_H = 160;
// Vẽ một frame xem trước 1s của hiệu ứng lên canvas thẻ
function renderCardFrame(canvas, eff, tMs) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const w = canvas.width || PREVIEW_W;
    const h = canvas.height || PREVIEW_H;
    // 1. Vẽ ảnh nền gốc hoannaoh.png
    if (isPreviewBgLoaded && previewBg.naturalWidth > 0) {
        ctx.drawImage(previewBg, 0, 0, w, h);
    } else {
        ctx.fillStyle = '#18181b';
        ctx.fillRect(0, 0, w, h);
    }
    // 2. Hiệu ứng đổi màu nền Background theo Bass
    if (eff.id === 'bg-bass') {
        const cycleProgress = (tMs % 1000) / 1000;
        const phase = cycleProgress < 0.5;
        ctx.fillStyle = phase ? 'rgba(255, 255, 255, 0.85)' : 'rgba(0, 0, 0, 0.85)';
        ctx.fillRect(0, 0, w, h);
        ctx.fillStyle = phase ? '#000' : '#fff';
        ctx.font = 'bold 13px Lexend, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('BASS', w / 2, h / 2);
        return;
    }
    // 3. Tạo cấu hình effect giả lập 1s
    const durMs = 1000;
    const item = eff.create(0, durMs);
    item.params = { ...item };
    // Nhịp bass giả lập để preview các hiệu ứng phản hồi âm thanh (Sound)
    if (item.source === 'bass') {
        const cycleProgress = (tMs % 1000) / 1000;
        const beatVal = Math.pow(Math.max(0, Math.sin(cycleProgress * Math.PI * 2)), 3);
        item.params.bass = {
            hop: 0.05,
            level: [beatVal, beatVal],
            hit: [1, 1]
        };
    }
    // 4. Áp dụng hiệu ứng qua TimelineEngine
    if (window.TimelineEngine && typeof window.TimelineEngine.applyEffect === 'function') {
        try {
            const sampleMs = (eff.id === 'intro' && tMs === 400) ? 700 : tMs;
            window.TimelineEngine.applyEffect(ctx, w, h, sampleMs, item, w, h);
        } catch (e) {
            // Không làm gián đoạn preview nếu shader chưa sẵn sàng
        }
    }
}
// Vẽ frame đại diện tĩnh ban đầu cho toàn bộ thẻ khi load
function renderAllCardThumbnails() {
    const container = document.getElementById('effect-list-container');
    if (!container) return;
    const cards = container.querySelectorAll('.effect-card');
    cards.forEach(card => {
        const id = card.dataset.effectId;
        const eff = AVAILABLE_EFFECTS.find(e => e.id === id);
        const canvas = card.querySelector('.effect-card-canvas');
        if (eff && canvas) {
            renderCardFrame(canvas, eff, 400);
        }
    });
}
let currentHoverCard = null;
let hoverRafId = null;
let hoverDebounceTimer = null;
// Chạy vòng lặp hoạt ảnh preview 1s khi hover chuột vào thẻ
function startHoverPreview(card, eff, canvas) {
    if (currentHoverCard === card) return;
    if (hoverDebounceTimer) {
        clearTimeout(hoverDebounceTimer);
        hoverDebounceTimer = null;
    }
    stopHoverPreview();
    // Micro-delay 50ms: nếu người dùng đang cuộn chuột lướt nhanh qua các thẻ,
    // không khởi động RAF preview để giữ tốc độ cuộn 60fps mượt mà, không giật lag.
    hoverDebounceTimer = setTimeout(() => {
        hoverDebounceTimer = null;
        currentHoverCard = card;
        const startT = performance.now();
        function loop(now) {
            if (currentHoverCard !== card) return;
            const tMs = (now - startT) % 1000;
            renderCardFrame(canvas, eff, tMs);
            hoverRafId = requestAnimationFrame(loop);
        }
        hoverRafId = requestAnimationFrame(loop);
    }, 50);
}
// Dừng hoạt ảnh preview và trả lại frame tĩnh
function stopHoverPreview() {
    if (hoverDebounceTimer) {
        clearTimeout(hoverDebounceTimer);
        hoverDebounceTimer = null;
    }
    if (hoverRafId) {
        cancelAnimationFrame(hoverRafId);
        hoverRafId = null;
    }
    if (currentHoverCard) {
        const prevCard = currentHoverCard;
        currentHoverCard = null;
        const id = prevCard.dataset.effectId;
        const eff = AVAILABLE_EFFECTS.find(e => e.id === id);
        const canvas = prevCard.querySelector('.effect-card-canvas');
        if (eff && canvas) {
            renderCardFrame(canvas, eff, 400);
        }
    }
}
let filteredEffects = AVAILABLE_EFFECTS;
let renderedEffectCount = 0;
const EFFECT_CHUNK_SIZE = 10;
let isEffectLoading = false;
let effectStreamGen = 0;
function createSingleEffectCard(eff) {
    const card = document.createElement('div');
    card.className = 'effect-card';
    card.dataset.effectId = eff.id;
    const previewWrap = document.createElement('div');
    previewWrap.className = 'effect-card-preview-wrap';
    const canvas = document.createElement('canvas');
    canvas.className = 'effect-card-canvas';
    canvas.width = PREVIEW_W;
    canvas.height = PREVIEW_H;
    previewWrap.appendChild(canvas);
    const title = document.createElement('span');
    title.className = 'effect-card-title';
    title.textContent = eff.name;
    title.title = eff.name;
    card.appendChild(previewWrap);
    card.appendChild(title);
    card.addEventListener('mouseenter', () => startHoverPreview(card, eff, canvas));
    card.addEventListener('mouseleave', () => stopHoverPreview());
    card.addEventListener('click', () => {
        addEffectToTimeline(eff, card);
    });
    if (isPreviewBgLoaded) {
        renderCardFrame(canvas, eff, 400);
    }
    return card;
}
function loadMoreEffects() {
    if (isEffectLoading) return;
    if (renderedEffectCount >= filteredEffects.length) return;
    const container = document.getElementById('effect-list-container');
    if (!container) return;
    isEffectLoading = true;
    const myGen = effectStreamGen;
    const startIdx = renderedEffectCount;
    const endIdx = Math.min(filteredEffects.length, startIdx + EFFECT_CHUNK_SIZE);
    let idx = startIdx;
    function streamNextEffectItem() {
        if (myGen !== effectStreamGen) {
            isEffectLoading = false;
            return;
        }
        if (idx < endIdx) {
            const card = createSingleEffectCard(filteredEffects[idx]);
            container.appendChild(card);
            idx++;
            renderedEffectCount = idx;
            requestAnimationFrame(streamNextEffectItem);
        } else {
            isEffectLoading = false;
            const scrollParent = container.closest('.tab-content') || container.parentElement;
            if (scrollParent &&
                scrollParent.clientHeight > 40 &&
                scrollParent.scrollHeight <= scrollParent.clientHeight + 40 &&
                renderedEffectCount < filteredEffects.length) {
                loadMoreEffects();
            }
        }
    }
    streamNextEffectItem();
}
function initEffectSearch() {
    const searchInput = document.getElementById('effect-search-input');
    if (!searchInput || searchInput._hasListener) return;
    searchInput._hasListener = true;
    searchInput.addEventListener('input', (e) => {
        effectStreamGen++;
        isEffectLoading = false;
        const query = e.target.value.trim().toLowerCase();
        const container = document.getElementById('effect-list-container');
        if (!container) return;
        if (!query) {
            filteredEffects = AVAILABLE_EFFECTS;
        } else {
            filteredEffects = AVAILABLE_EFFECTS.filter(eff =>
                eff.name.toLowerCase().includes(query) ||
                eff.tag.toLowerCase().includes(query) ||
                eff.id.toLowerCase().includes(query)
            );
        }
        container.innerHTML = '';
        renderedEffectCount = 0;
        if (filteredEffects.length === 0) {
            const emptyEl = document.createElement('div');
            emptyEl.id = 'effect-search-empty';
            emptyEl.className = 'tab-search-empty';
            emptyEl.textContent = 'Không tìm thấy hiệu ứng phù hợp';
            container.appendChild(emptyEl);
            return;
        }
        loadMoreEffects();
    });
}
function initEffectsList() {
    const container = document.getElementById('effect-list-container');
    if (!container) return;
    effectStreamGen++;
    isEffectLoading = false;
    filteredEffects = AVAILABLE_EFFECTS;
    renderedEffectCount = 0;
    container.innerHTML = '';
    initEffectSearch();
    loadMoreEffects();
    const scrollParent = container.closest('.tab-content') || container.parentElement;
    if (scrollParent && !scrollParent._hasEffectScrollListener) {
        scrollParent._hasEffectScrollListener = true;
        scrollParent.addEventListener('scroll', () => {
            if (scrollParent.scrollHeight - (scrollParent.scrollTop + scrollParent.clientHeight) <= 180) {
                loadMoreEffects();
            }
        }, { passive: true });
    }
    if (!isPreviewBgLoaded) {
        let retries = 0;
        const checkReady = () => {
            if (isPreviewBgLoaded && window.TimelineEngine) {
                renderAllCardThumbnails();
            } else if (retries < 30) {
                retries++;
                setTimeout(checkReady, 100);
            }
        };
        checkReady();
    }
}
function addEffectToTimeline(eff, elItem) {
    const curMs = Math.max(0, Math.round(getAccurateTimeMs()));
    const start = curMs;
    const end = curMs + 1000;
    const newEffect = eff.create(start, end);
    const rawTimeline = (typeof window.getProjectTabData === 'function')
        ? window.getProjectTabData('timeline')
        : (state && Array.isArray(state.timeline) ? state.timeline : []);
    const timelineList = Array.isArray(rawTimeline) ? [...rawTimeline] : [];
    // Chèn vào vị trí phù hợp theo thứ tự start
    let insertIdx = timelineList.length;
    for (let i = 0; i < timelineList.length; i++) {
        const it = timelineList[i];
        const s = (it && typeof it.start === 'number') ? it.start : 0;
        if (s > start) {
            insertIdx = i;
            break;
        }
    }
    timelineList.splice(insertIdx, 0, newEffect);
    const targetSnippet = newEffect.effect
        ? `"effect": "${newEffect.effect}"`
        : (newEffect.source === 'bass' ? `"source": "bass"` : `"type": "${newEffect.type}"`);
    if (typeof window.updateProjectTabData === 'function') {
        window.updateProjectTabData('timeline', timelineList, {
            highlightSnippet: targetSnippet,
            startMs: start,
            focusEditor: false
        });
    }
    if (elItem) {
        elItem.classList.add('added-flash');
        setTimeout(() => elItem.classList.remove('added-flash'), 400);
    }
    // Cập nhật thông báo thanh trạng thái
    if (UI.statusText) {
        const prevText = UI.statusText.innerText;
        const durSec = Math.round((newEffect.end - newEffect.start) / 1000);
        UI.statusText.innerText = `✓ Đã thêm ${eff.name} (+${durSec}s tại ${start}ms)`;
        UI.statusText.classList.add('is-success');
        setTimeout(() => {
            if (UI.statusText.innerText.startsWith('✓ Đã thêm hiệu ứng')) {
                UI.statusText.innerText = prevText;
                UI.statusText.classList.remove('is-success');
            }
        }, 2500);
    }
}
// Khởi chạy danh sách effect
initEffectsList();
