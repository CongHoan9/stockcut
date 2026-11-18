// ============================================================================
// FONTS — Tải font dựa trên Asset/font/fonts.json + Panel quản trị font
//   Phụ thuộc : ui.js, editor (qua sự kiện 'input' của UI.jsonEditor)
//   Cung cấp  : loadGoogleFont, loadFontList, applyFontToConfig
//   Đặc điểm  :
//     • Đọc 100% theo manifest fonts.json (Zero-Bulk-Font-Load: KHÔNG tải toàn bộ kho .ttf lúc mở trang).
//     • Chỉ tải nhị phân .ttf/.otf on-demand khi project hoặc người dùng sử dụng họ font đó.
//     • Danh sách Font tải lười theo từng chunk (15 items/chunk), tải xong item nào appendChild ngay vào cha,
//       chỉ khi scroll tới đáy mới tải tiếp chunk kế tiếp.
// ============================================================================
let localFontCatalogLoaded = false;
const NativeFontAssets = (() => {
    let entries = [];
    const cssFontRequests = new Map();
    const binaryFontRequests = new Map();
    const MANIFEST_PATH = 'Asset/font/fonts.json';
    const ensureSingleBinaryFile = (filePath) => {
        if (!filePath) return Promise.resolve(false);
        if (binaryFontRequests.has(filePath)) return binaryFontRequests.get(filePath);
        const req = (async () => {
            try {
                if (window.WasmBridge && window.WasmBridge.whenReady) {
                    await window.WasmBridge.whenReady();
                }
                const fileResp = await fetch(new URL(filePath, document.baseURI).href);
                if (fileResp.ok && window.WasmBridge && window.WasmBridge.fontMap) {
                    const buf = await fileResp.arrayBuffer();
                    window.WasmBridge.fontMap.registerFile(filePath, new Uint8Array(buf));
                    return true;
                }
            } catch (err) {
                console.warn(`[NativeFontAssets] Lỗi khi nạp file font nhị phân "${filePath}":`, err);
            }
            return false;
        })();
        binaryFontRequests.set(filePath, req);
        return req;
    };
    // 1. Chỉ phân tích fonts.json (vài KB), KHÔNG chờ tải toàn bộ kho .ttf
    const initFromManifest = async () => {
        try {
            const manifestUrl = new URL(MANIFEST_PATH, document.baseURI).href;
            const resp = await fetch(manifestUrl);
            if (!resp.ok) {
                console.error(`[NativeFontAssets] Không thể tải ${MANIFEST_PATH}: HTTP ${resp.status}`);
                return [];
            }
            const jsonText = await resp.text();
            // Đẩy chuỗi JSON vào C++ FontMapRegistry ngay khi Wasm sẵn sàng
            if (window.WasmBridge && window.WasmBridge.whenReady) {
                try {
                    await window.WasmBridge.whenReady();
                    if (window.WasmBridge.fontMap) {
                        window.WasmBridge.fontMap.loadManifest(jsonText);
                    }
                } catch (_) {}
            }
            const familyList = JSON.parse(jsonText);
            const resultEntries = [];
            for (const item of familyList) {
                const familyName = item.family;
                const dirName = item.dir || familyName;
                const variants = item.variants || [];
                if (!familyName || variants.length === 0) continue;
                const variantFiles = [];
                const resolvedVariants = [];
                for (const v of variants) {
                    const filePath = v.file.startsWith('Asset/font/')
                        ? v.file
                        : `Asset/font/${dirName}/${v.file}`;
                    if (!variantFiles.includes(filePath)) {
                        variantFiles.push(filePath);
                    }
                    resolvedVariants.push({
                        weight: Number(v.weight || 400),
                        italic: Boolean(v.italic),
                        file: filePath
                    });
                }
                let bestVar = resolvedVariants[0];
                let bestDist = 99999;
                for (const v of resolvedVariants) {
                    const dist = Math.abs(v.weight - 450) + (v.italic ? 1000 : 0);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestVar = v;
                    }
                }
                const previewFile = bestVar.file;
                const ext = previewFile.slice(previewFile.lastIndexOf('.') + 1).toLowerCase() || 'ttf';
                resultEntries.push({
                    family: familyName,
                    dir: dirName,
                    displayName: familyName,
                    fontPath: familyName,
                    extension: ext,
                    url: previewFile ? new URL(previewFile, document.baseURI).href : '',
                    src: previewFile,
                    variantFiles,
                    variants: resolvedVariants
                });
            }
            return resultEntries;
        } catch (err) {
            console.error('[NativeFontAssets] Lỗi khởi tạo danh mục font:', err);
            return [];
        }
    };
    const ready = initFromManifest()
        .then(fonts => {
            entries = fonts;
            localFontCatalogLoaded = true;
            // Preload only the primary regular font file (1 file, not all variants)
            const defaultFont = entries.length > 0 ? entries[0] : null;
            if (defaultFont && defaultFont.src) {
                ensureSingleBinaryFile(defaultFont.src);
            }
            if (window.state && window.state.timeline && window.TimelineEngine && typeof window.TimelineEngine.preloadProjectFonts === 'function') {
                window.TimelineEngine.preloadProjectFonts(window.state.timeline, window.state.templates || []);
            }
            return entries;
        })
        .catch(err => {
            console.error('[NativeFontAssets] Thất bại khi load danh mục font:', err);
            entries = [];
            localFontCatalogLoaded = true;
            return entries;
        });
    return {
        ready,
        get loaded() {
            return localFontCatalogLoaded;
        },
        getDefaultFamily() {
            return (entries.length > 0 && entries[0].family) ? entries[0].family : '';
        },
        getEntries() {
            return [...entries];
        },
        findByFontPath(fontPath) {
            const path = String(fontPath || '').trim();
            if (!path || path === 'sans-serif' || path === 'default' || path === 'system-ui') {
                return (entries.length > 0 ? entries[0] : null);
            }
            let found = entries.find(f => f.fontPath === path || f.family === path || f.dir === path || f.src === path);
            if (found) return found;
            const normalize = (s) => String(s || '')
                .replace(/^\/?(Asset\/)?font\//i, '')
                .split(/[\/\\]/)[0]
                .replace(/\.(ttf|otf|woff2|woff)$/i, '')
                .replace(/[-_\s]/g, '')
                .replace(/variablefont(wght|wdth)$/i, '')
                .toLowerCase();
            const normTarget = normalize(path);
            found = entries.find(f => normalize(f.family) === normTarget || normalize(f.dir) === normTarget);
            if (found) return found;
            found = entries.find(f => Array.isArray(f.variantFiles) && f.variantFiles.some(file => file === path || file.endsWith('/' + path)));
            if (found) return found;
            return (entries.length > 0 ? entries[0] : null);
        },
        async ensureNativeBinary(fontOrFamily) {
            await ready;
            const font = (typeof fontOrFamily === 'string')
                ? this.findByFontPath(fontOrFamily)
                : fontOrFamily;
            if (!font || !Array.isArray(font.variantFiles)) return false;
            await Promise.all(font.variantFiles.map(fp => ensureSingleBinaryFile(fp)));
            return true;
        },
        // Tải CSS Font on-demand cho riêng chữ preview "ART" trong thẻ mà không ảnh hưởng UI Lexend
        async loadCssFont(font, familyName = (font && font.family) || 'CustomFont', descriptors = {}) {
            if (!font) return false;
            const targetUrl = font.url || (font.src ? new URL(font.src, document.baseURI).href : '');
            if (!targetUrl || typeof FontFace === 'undefined') return false;
            if (cssFontRequests.has(familyName)) return cssFontRequests.get(familyName);
            const req = (async () => {
                try {
                    if (document.fonts && document.fonts.check(`12px "${familyName}"`)) return true;
                    const face = new FontFace(familyName, `url("${targetUrl}")`, descriptors);
                    const loadedFace = await face.load();
                    document.fonts.add(loadedFace);
                    return true;
                } catch (_) {
                    return false;
                }
            })();
            cssFontRequests.set(familyName, req);
            return req;
        }
    };
})();
window.NativeFontAssets = NativeFontAssets;
// Tải font on-demand cho project (vừa nạp CSS Font vừa nạp đúng file .ttf vào C++ FontMapRegistry)
async function loadGoogleFont(fontFamily) {
    // 100% Native font handling in C++ Core
    return Promise.resolve();
}
let allFonts = [];
let filteredFonts = [];
let renderedFontCount = 0;
const FONT_PAGE_SIZE = 15;
let isFontLoading = false;
let fontStreamGeneration = 0;
function appendSingleFontItem(container, font) {
    const primaryFont = font.family;
    NativeFontAssets.loadCssFont(font, primaryFont).catch(() => {});

    const el = document.createElement('div');
    el.className = 'font-card effect-card';
    el.dataset.family = primaryFont;
    el.title = `Click để áp dụng font ${primaryFont}`;
    el.innerHTML = `
        <div class="font-card-preview-wrap effect-card-preview-wrap">
            <div class="font-card-preview" style="font-family: '${primaryFont}', sans-serif;">ART</div>
        </div>
        <div class="font-card-title effect-card-title" title="${primaryFont}">${primaryFont}</div>
    `;

    el.addEventListener('click', (e) => {
        if (e) e.stopPropagation();
        container.querySelectorAll('.font-card').forEach(c => c.classList.remove('active'));
        el.classList.add('active');
        el.classList.add('added-flash');
        setTimeout(() => el.classList.remove('added-flash'), 400);
        applyFontToConfig(font);
    });

    container.appendChild(el);
}
function getActiveFontList() {
    const searchInput = document.getElementById('font-search-input');
    if (searchInput && searchInput.value.trim().length > 0) {
        return filteredFonts;
    }
    return allFonts;
}
// Tải lười từng chunk 15 items; trong mỗi chunk, tạo xong item nào appendChild ngay vào cha
function loadMoreFonts() {
    if (isFontLoading) return;
    const currentList = getActiveFontList();
    if (renderedFontCount >= currentList.length) return;
    const container = UI.fontListContainer || document.getElementById('font-list-container');
    if (!container) return;
    isFontLoading = true;
    const myGen = fontStreamGeneration;
    const startIdx = renderedFontCount;
    const endIdx = Math.min(currentList.length, startIdx + FONT_PAGE_SIZE);
    let idx = startIdx;
    function streamNextFontItem() {
        if (myGen !== fontStreamGeneration) {
            isFontLoading = false;
            return;
        }
        if (idx < endIdx) {
            appendSingleFontItem(container, currentList[idx]);
            idx++;
            renderedFontCount = idx;
            requestAnimationFrame(streamNextFontItem);
        } else {
            isFontLoading = false;
            // CHỈ tải thêm nếu panel đang mở (clientHeight > 40) và danh sách chưa đủ tạo thanh cuộn
            const scrollParent = container.closest('.pane-scroll-area') || container.closest('.tab-content') || container.parentElement;
            if (scrollParent &&
                scrollParent.clientHeight > 40 &&
                scrollParent.scrollHeight <= scrollParent.clientHeight + 40 &&
                renderedFontCount < currentList.length) {
                loadMoreFonts();
            }
        }
    }
    streamNextFontItem();
}
function initFontSearch() {
    const searchInput = document.getElementById('font-search-input');
    if (!searchInput || searchInput._hasListener) return;
    searchInput._hasListener = true;
    searchInput.addEventListener('input', (e) => {
        fontStreamGeneration++;
        isFontLoading = false;
        const query = e.target.value.trim().toLowerCase();
        if (!query) {
            filteredFonts = allFonts;
        } else {
            filteredFonts = allFonts.filter(font =>
                font.family.toLowerCase().includes(query) || font.src.toLowerCase().includes(query));
        }
        const container = UI.fontListContainer || document.getElementById('font-list-container');
        if (!container) return;
        container.innerHTML = '';
        renderedFontCount = 0;
        
        if (filteredFonts.length === 0) {
            container.innerHTML = '<div class="tab-search-empty">Không tìm thấy font chữ phù hợp</div>';
            return;
        }
        loadMoreFonts();
    });
}
async function loadFontList() {
    const container = UI.fontListContainer || document.getElementById('font-list-container');
    if (!container) return;
    try {
        allFonts = await NativeFontAssets.ready;
        filteredFonts = allFonts;
        fontStreamGeneration++;
        isFontLoading = false;
        container.innerHTML = '';
        renderedFontCount = 0;
        
        initFontSearch();
        if (allFonts.length === 0) {
            container.textContent = 'Không tìm thấy font trong Asset/font/fonts.json.';
            return;
        }
        loadMoreFonts();
        const scrollParent = container.closest('.pane-scroll-area') || container.closest('.tab-content') || container.parentElement;
        if (scrollParent && !scrollParent._hasFontScrollListener) {
            scrollParent._hasFontScrollListener = true;
            scrollParent.addEventListener('scroll', () => {
                if (scrollParent.scrollHeight - (scrollParent.scrollTop + scrollParent.clientHeight) <= 180) {
                    loadMoreFonts();
                }
            }, { passive: true });
        }
    } catch (e) {
        const err = document.createElement('p');
        err.className = 'font-list-error';
        err.textContent = `Không thể nạp font: ${e.message}`;
        container.replaceChildren(err);
    }
}
window.ensureFontListLoaded = function () {
    const container = UI.fontListContainer || document.getElementById('font-list-container');
    if (!container) return;
    if (allFonts.length === 0 || container.children.length === 0) {
        loadFontList();
    } else {
        loadMoreFonts();
    }
};
async function applyFontToConfig(font, variantStyle = null) {
    try {
        const targetFamilyName = (typeof font === 'string') ? font : (font && font.family ? font.family : '');
        if (!targetFamilyName) return;
        if (typeof font === 'object' && font) {
            NativeFontAssets.loadCssFont(font, targetFamilyName).catch(error => {
                console.error(`[Fonts] Lỗi khi nạp preview "${font.src}":`, error);
            });
        }
        await NativeFontAssets.ensureNativeBinary(font);

        const rawTemplate = (typeof window.getProjectTabData === 'function')
            ? window.getProjectTabData('template')
            : (window.state && Array.isArray(window.state.templates) ? window.state.templates : []);
        const tmplList = Array.isArray(rawTemplate) ? [...rawTemplate] : [];

        const updateItem = (item) => {
            item['font-family'] = targetFamilyName;
            delete item.fontFamily;
            if (variantStyle && variantStyle.weight) {
                item['font-weight'] = Number(variantStyle.weight) || variantStyle.weight;
                delete item.fontWeight;
            } else {
                delete item['font-weight'];
                delete item.fontWeight;
            }
            if (variantStyle && variantStyle.style) {
                item['font-style'] = variantStyle.style;
                delete item.fontStyle;
            } else {
                delete item['font-style'];
                delete item.fontStyle;
            }
            for (const subKey of ['style', 'css', 'params']) {
                if (item[subKey] && typeof item[subKey] === 'object') {
                    delete item[subKey]['font-family'];
                    delete item[subKey].fontFamily;
                    delete item[subKey]['font-weight'];
                    delete item[subKey].fontWeight;
                    delete item[subKey]['font-style'];
                    delete item[subKey].fontStyle;
                }
            }
        };

        let lyricStyle = tmplList.find(it => it && String(it.type).toLowerCase() === 'lyric' && !it.class && !it.name && !it.id);
        if (lyricStyle) {
            updateItem(lyricStyle);
        } else {
            const newLyric = { type: "lyric" };
            updateItem(newLyric);
            tmplList.push(newLyric);
        }

        if (typeof window.updateProjectTabData === 'function') {
            window.updateProjectTabData('template', tmplList, {
                highlightSnippet: `"font-family": "${targetFamilyName}"`,
                focusEditor: false,
                debounceMs: 0
            });
        }

        if (window.WasmBridge && window.WasmBridge.renderer && typeof window.WasmBridge.renderer.invalidateTextCaches === 'function') {
            window.WasmBridge.renderer.invalidateTextCaches();
        }
        if (typeof window.updateFrame === 'function') {
            window.updateFrame();
        }
        if (window.UI && UI.statusText) {
            UI.statusText.innerText = `✓ Đã áp dụng font "${targetFamilyName}" vào template.json`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => UI.statusText.classList.remove('is-success'), 2500);
        }
    } catch (e) {
        console.error('[Fonts] applyFontToConfig error:', e);
    }
}
window.applyFontToLyricTemplate = applyFontToConfig;
// Khởi chạy độc lập (không chặn bất kỳ danh sách nào khác)
loadFontList();
// ============================================================================
// BUNE STUDIO - TEXT TEMPLATES & BASIC TEXT ADD BUTTONS
// ============================================================================
const TEXT_TEMPLATES = [
    { text: "PERFECTO", style: "color: #fff; font-family: 'Anton', sans-serif; text-shadow: 2px 2px #ff4757;" },
    { text: "SUPER", style: "color: #ffd32a; font-family: 'Righteous', sans-serif; text-shadow: 0 0 8px #ff3f34;" },
    { text: "VIBES", style: "color: #ff3f34; font-family: 'Pacifico', cursive;" },
    { text: "CINEMA", style: "color: #00d8d6; font-family: 'Orbitron', sans-serif; letter-spacing: 2px;" },
    { text: "GLOW", style: "color: #ff5e57; font-family: 'Bebas Neue', sans-serif; text-shadow: 0 0 12px #ff5e57;" },
    { text: "GOLDEN", style: "color: #ffa801; font-family: 'Cinzel', serif;" }
];
function addTextLayerToTimeline(isHeading = true, customFont = null) {
    const curMs = Math.max(0, Math.round(typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : (window.state?.currentTimeMs || 0)));
    const start = curMs;
    const end = start + 5000;
    const rawTimeline = (typeof window.getProjectTabData === 'function')
        ? window.getProjectTabData('timeline')
        : (window.state && Array.isArray(window.state.timeline) ? window.state.timeline : []);
    const timelineList = Array.isArray(rawTimeline) ? [...rawTimeline] : [];
    const newTextItem = {
        type: "text",
        text: isHeading ? "Add heading" : "Add body text",
        start: start,
        end: end,
        left: "50%",
        top: "50%",
        transform: "translate(-50%, -50%)",
        fontSize: isHeading ? 64 : 32,
        fontWeight: isHeading ? "700" : "400",
        fontFamily: customFont || "Montserrat",
        color: "#ffffff",
        zIndex: 10
    };
    let insertIdx = timelineList.length;
    for (let i = 0; i < timelineList.length; i++) {
        const it = timelineList[i];
        const s = (it && typeof it.start === 'number') ? it.start : 0;
        if (s > start) {
            insertIdx = i;
            break;
        }
    }
    timelineList.splice(insertIdx, 0, newTextItem);
    if (typeof window.updateProjectTabData === 'function') {
        window.updateProjectTabData('timeline', timelineList, {
            highlightSnippet: `"text": "${newTextItem.text}"`,
            startMs: start,
            focusEditor: false
        });
    }
}
function applyFontFamilyToActiveText(fontFamily) {
    applyFontToConfig(fontFamily);
}
function renderFontArtGrids() {
    const templatesGrid = document.getElementById('text-templates-grid');
    if (templatesGrid && templatesGrid.children.length === 0) {
        TEXT_TEMPLATES.forEach(t => {
            const card = document.createElement('div');
            card.className = 'text-template-card';
            card.title = `Click để thêm mẫu chữ "${t.text}"`;
            card.innerHTML = `<div class="text-tmpl-preview" style="${t.style}">${t.text}</div>`;
            card.addEventListener('click', () => {
                addTextLayerToTimeline(true, t.text);
            });
            templatesGrid.appendChild(card);
        });
    }
    if (typeof window.ensureFontListLoaded === 'function') {
        window.ensureFontListLoaded();
    }
}
// Khởi tạo các nút Basic Text (Add heading & Add body text)
function initTextAddButtons() {
    const btnHeading = document.getElementById('btn-add-heading');
    if (btnHeading && !btnHeading._hasAddListener) {
        btnHeading._hasAddListener = true;
        btnHeading.addEventListener('click', () => {
            addTextLayerToTimeline(true);
        });
    }
    const btnBody = document.getElementById('btn-add-body');
    if (btnBody && !btnBody._hasAddListener) {
        btnBody._hasAddListener = true;
        btnBody.addEventListener('click', () => {
            addTextLayerToTimeline(false);
        });
    }
    renderFontArtGrids();
}
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initTextAddButtons);
} else {
    initTextAddButtons();
}
window.renderFontArtGrids = renderFontArtGrids;
