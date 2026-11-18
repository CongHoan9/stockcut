// ============================================================================
// JSON EDITOR — live preview, phím Tab, Ctrl+S lưu trực tiếp, tìm kiếm từ khóa
//   Phụ thuộc : ui.js, state.js, timeline-new.js, render-config.js, viewport.js, preview.js, waveform.js
//   Cung cấp  : saveJsonDirectly, jumpToJsonMatch, searchKeyword
// ============================================================================
// ============================================================================
// CHUYÊN BIỆT HÓA 4 TAB ĐỘC LẬP TUYỆT ĐỐI: CONFIG | TEMPLATE | TIMELINE | STYLE
//   - Sự kiện chuyển tab (switchJsonTab) CHỈ đổi buffer văn bản hiển thị trên UI,
//     TUYỆT ĐỐI KHÔNG gọi WasmBridge.doEdit, KHÔNG reset hay can thiệp luồng Render.
//   - Mọi thao tác thêm Media/Audio/Effect luôn ghi đích danh vào tab 'timeline'.
//   - Mọi thao tác chọn Font Family luôn ghi đích danh vào tab 'template'.
//   - Mọi thao tác kịch bản giao diện/rule luôn ghi đích danh vào tab 'style'.
// ============================================================================
let currentJsonTab = 'timeline'; // Mặc định hiển thị timeline theo luồng dựng video
let fullProjectData = {
    config: { aspectRatio: "original" },
    template: [],
    timeline: [],
    style: []
};
let tabTextBuffers = {
    config: '',
    template: '',
    timeline: '',
    style: ''
};
let tabHasError = {
    config: false,
    template: false,
    timeline: false,
    style: false
};
let tabFoldedStarts = {
    config: new Set(),
    template: new Set(),
    timeline: new Set(),
    style: new Set()
};
let isTabsInitializedFromRam = false;

function syncFullProjectFromRam(rawJsonOverride = null) {
    try {
        const rawSource = rawJsonOverride !== null ? rawJsonOverride : (state.ram && state.ram.projectJson);
        if (rawSource) {
            const parsed = JSON.parse(rawSource);
            if (parsed && typeof parsed === 'object') {
                fullProjectData.config = parsed.config || state.config || { aspectRatio: "original" };
                fullProjectData.template = Array.isArray(parsed.template)
                    ? parsed.template
                    : (Array.isArray(parsed.templates) ? parsed.templates : (state.templates || []));
                fullProjectData.timeline = Array.isArray(parsed.timeline) ? parsed.timeline : (state.timeline || []);
                fullProjectData.style = Array.isArray(parsed.style)
                    ? parsed.style
                    : (Array.isArray(parsed.roles) ? parsed.roles : (Array.isArray(parsed.styles) ? parsed.styles : []));
            }
        } else {
            fullProjectData.config = state.config || { aspectRatio: "original" };
            fullProjectData.template = Array.isArray(state.templates) ? state.templates : [];
            fullProjectData.timeline = Array.isArray(state.timeline) ? state.timeline : [];
            fullProjectData.style = [];
        }
    } catch (_) {
        fullProjectData.config = state.config || {};
        fullProjectData.template = Array.isArray(state.templates) ? state.templates : [];
        fullProjectData.timeline = Array.isArray(state.timeline) ? state.timeline : [];
        fullProjectData.style = [];
    }
    if (fullProjectData.config && fullProjectData.config.lyricSize !== undefined) {
        delete fullProjectData.config.lyricSize;
    }
    if (state.config && state.config.lyricSize !== undefined) {
        delete state.config.lyricSize;
    }
    tabTextBuffers.config = JSON.stringify(fullProjectData.config || {}, null, " ");
    tabTextBuffers.template = JSON.stringify(fullProjectData.template || [], null, " ");
    tabTextBuffers.timeline = JSON.stringify(fullProjectData.timeline || [], null, " ");
    tabTextBuffers.style = JSON.stringify(fullProjectData.style || [], null, " ");
    tabHasError.config = false;
    tabHasError.template = false;
    tabHasError.timeline = false;
    tabHasError.style = false;
    tabFoldedStarts.config.clear();
    tabFoldedStarts.template.clear();
    tabFoldedStarts.timeline.clear();
    tabFoldedStarts.style.clear();
    isTabsInitializedFromRam = true;
}

function syncProjectTabsFromRam(rawJsonOverride = null) {
    syncFullProjectFromRam(rawJsonOverride);
    if (UI.jsonEditor) {
        UI.jsonEditor.value = tabTextBuffers[currentJsonTab] || formatTabSlice(currentJsonTab);
        UI.jsonEditor.classList.remove('error');
    }
}
window.syncProjectTabsFromRam = syncProjectTabsFromRam;

function getTabSlice(tab) {
    if (!isTabsInitializedFromRam) {
        syncFullProjectFromRam();
    }
    if (tab === 'config') {
        return fullProjectData.config || {};
    } else if (tab === 'template') {
        return Array.isArray(fullProjectData.template) ? fullProjectData.template : [];
    } else if (tab === 'style') {
        return Array.isArray(fullProjectData.style) ? fullProjectData.style : [];
    } else {
        return Array.isArray(fullProjectData.timeline) ? fullProjectData.timeline : [];
    }
}

function formatTabSlice(tab) {
    return JSON.stringify(getTabSlice(tab), null, " ");
}

function getProjectTabData(tab) {
    const slice = getTabSlice(tab);
    try {
        return JSON.parse(JSON.stringify(slice));
    } catch (_) {
        return slice;
    }
}
window.getProjectTabData = getProjectTabData;

function recordTabHistoryStep(tab, sliceData) {
    if (!window.WasmBridge || !window.WasmBridge.isReady() || !window.WasmBridge.doEdit) return;
    if (tab === 'config') {
        window.WasmBridge.doEdit(2, JSON.stringify(sliceData));
    } else if (tab === 'template') {
        window.WasmBridge.doEdit(1, JSON.stringify(sliceData));
    } else if (tab === 'style') {
        window.WasmBridge.doEdit(0, JSON.stringify(fullProjectData));
    } else if (tab === 'timeline') {
        window.WasmBridge.doEdit(0, JSON.stringify(fullProjectData));
    }
}

function tryFlushCurrentEditorToRam() {
    if (!UI.jsonEditor) return;
    const text = UI.jsonEditor.value;
    tabTextBuffers[currentJsonTab] = text;
    if (!text || !text.trim()) return;
    try {
        const parsed = JSON.parse(text);
        tabHasError[currentJsonTab] = false;
        if (currentJsonTab === 'config' && parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
            fullProjectData.config = parsed;
            state.config = parsed;
        } else if (currentJsonTab === 'template' && Array.isArray(parsed)) {
            fullProjectData.template = parsed;
            state.templates = parsed;
        } else if (currentJsonTab === 'style' && Array.isArray(parsed)) {
            fullProjectData.style = parsed;
        } else if (currentJsonTab === 'timeline' && Array.isArray(parsed)) {
            fullProjectData.timeline = parsed;
        }
        state.ram.projectJson = JSON.stringify(fullProjectData, null, " ");
    } catch (_) {
        tabHasError[currentJsonTab] = true;
    }
}

function switchJsonTab(tabName) {
    if (!tabName) return;
    if (!isTabsInitializedFromRam) {
        syncFullProjectFromRam();
    }
    // Chỉ lưu lại nội dung chữ đang gõ ở tab cũ vào bộ đệm của riêng tab đó,
    // TUYỆT ĐỐI KHÔNG gọi WasmBridge.doEdit hay can thiệp vào Engine/Render!
    if (UI.jsonEditor && currentJsonTab) {
        tabTextBuffers[currentJsonTab] = UI.jsonEditor.value;
    }
    currentJsonTab = tabName;
    document.querySelectorAll('.editor-tab-btn').forEach(btn => {
        const t = btn.getAttribute('data-json-tab');
        const isActive = t === currentJsonTab;
        btn.classList.toggle('active', isActive);
        btn.setAttribute('aria-selected', isActive ? 'true' : 'false');
    });
    if (UI.jsonEditor) {
        if (!tabTextBuffers[currentJsonTab]) {
            tabTextBuffers[currentJsonTab] = formatTabSlice(currentJsonTab);
        }
        UI.jsonEditor.value = tabTextBuffers[currentJsonTab];
        UI.jsonEditor.classList.toggle('error', Boolean(tabHasError[currentJsonTab]));
    }
    const scopeEl = UI.jsonSearchScope || document.getElementById('json-search-scope');
    if (tabName !== 'timeline' && scopeEl && scopeEl.value !== 'all') {
        scopeEl.value = 'all';
        if (typeof updatePlaceholder === 'function') updatePlaceholder();
    }
    const searchInput = UI.jsonSearchInput || document.getElementById('json-search-input');
    if (searchInput && searchInput.value) {
        searchKeyword(searchInput.value, 0, false);
    }
}
window.setJsonEditorTab = switchJsonTab;
function initJsonTabs() {
    const tabBtns = document.querySelectorAll('.editor-tab-btn');
    tabBtns.forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.preventDefault();
            const tabName = btn.getAttribute('data-json-tab');
            switchJsonTab(tabName);
        });
    });
}
if (document.readyState === 'loading') {
    window.addEventListener('DOMContentLoaded', initJsonTabs);
} else {
    initJsonTabs();
}
// 6. Cập nhật JSON Editor (Live Preview) với Debounce chống giật lag
let jsonParseTimeout = null;
let sessionSnapshotTimeout = null;
function queueSessionSnapshotSave() {
    clearTimeout(sessionSnapshotTimeout);
    sessionSnapshotTimeout = setTimeout(() => {
        try {
            const parsed = JSON.parse(state.ram.projectJson);
            if (!Array.isArray(parsed.timeline)) return;
        } catch (_) {
            return;
        }
        if (typeof persistSessionProject === 'function') {
            persistSessionProject(state.sessionProjectName, state.ram.projectJson, state.ram.lyricsTxt);
        }
    }, 400);
}

function scheduleEngineSync(delayMs = 50) {
    clearTimeout(jsonParseTimeout);
    jsonParseTimeout = setTimeout(async () => {
        try {
            state.config = fullProjectData.config || {};
            state.templates = Array.isArray(fullProjectData.template) ? fullProjectData.template : [];

            const curRatio = state.config.aspectRatio || 'original';
            if (typeof window.updateAspectRatioBtn === 'function') {
                window.updateAspectRatioBtn(curRatio);
            }
            const ratioPanel = document.getElementById('aspect-ratio-panel');
            if (ratioPanel) {
                ratioPanel.querySelectorAll('.ratio-option').forEach(o => {
                    o.classList.toggle('active', o.dataset.ratio === curRatio);
                });
            }

            if (typeof buildRenderConfig === 'function') {
                await buildRenderConfig();
            }

            if (typeof window.restoreViewportFromConfig === 'function') {
                window.restoreViewportFromConfig();
            } else if (typeof initViewportSize === 'function') {
                initViewportSize();
            }

            const fullJsonStr = JSON.stringify(fullProjectData, null, " ");
            state.ram.projectJson = fullJsonStr;
            state.timeline = (TimelineEngine.applyJsonMove
                ? TimelineEngine.applyJsonMove(fullProjectData, fullJsonStr)
                : TimelineEngine.parseProject(fullProjectData, fullJsonStr)).timeline;
            updateFrame(getAccurateTimeMs());
            if (typeof initVirtualPlayerUI === 'function') initVirtualPlayerUI();
            if (state.projectContext && state.projectContext.baseUrl && typeof window.registerProjectBaseUrlAssets === 'function') {
                window.registerProjectBaseUrlAssets(state.timeline, state.projectContext.baseUrl);
            }
            TimelineEngine.preloadAssets(state.dirHandle || null).then(() => {
                refreshTimelineWaveform();
                if (!state.isPlaying) updateFrame(getAccurateTimeMs());
            }).catch(() => {});
        } catch (e) {
            console.error('[JSON Editor Parse / Sync Error]', e);
        }
    }, delayMs);
}

function updateProjectTabData(tabName, newSliceData, options = {}) {
    if (!isTabsInitializedFromRam) {
        syncFullProjectFromRam();
    }
    if (tabName === 'config') {
        fullProjectData.config = (newSliceData && typeof newSliceData === 'object' && !Array.isArray(newSliceData)) ? newSliceData : {};
        if (fullProjectData.config.lyricSize !== undefined) delete fullProjectData.config.lyricSize;
        if (state.config && state.config.lyricSize !== undefined) delete state.config.lyricSize;
        state.config = fullProjectData.config;
    } else if (tabName === 'template') {
        fullProjectData.template = Array.isArray(newSliceData) ? newSliceData : [];
        if (fullProjectData.templates) fullProjectData.templates = fullProjectData.template;
        state.templates = fullProjectData.template;
    } else if (tabName === 'style') {
        fullProjectData.style = Array.isArray(newSliceData) ? newSliceData : [];
    } else if (tabName === 'timeline') {
        fullProjectData.timeline = Array.isArray(newSliceData) ? newSliceData : [];
    } else {
        return;
    }

    const formattedSlice = JSON.stringify(fullProjectData[tabName], null, " ");
    tabTextBuffers[tabName] = formattedSlice;
    tabHasError[tabName] = false;

    recordTabHistoryStep(tabName, fullProjectData[tabName]);
    state.ram.projectJson = JSON.stringify(fullProjectData, null, " ");
    state.ram.isJsonDirty = true;
    queueSessionSnapshotSave();

    // Chỉ cập nhật nội dung khung soạn thảo nếu người dùng ĐANG mở đúng tab đó;
    // Nếu đang mở tab khác thì giữ nguyên tab đang mở, hoàn toàn độc lập!
    if (currentJsonTab === tabName && UI.jsonEditor) {
        UI.jsonEditor.value = formattedSlice;
        UI.jsonEditor.classList.remove('error');
        UI.jsonEditor.classList.add('flash-ok');
        setTimeout(() => UI.jsonEditor.classList.remove('flash-ok'), 500);

        if (options.highlightSnippet) {
            setTimeout(() => {
                const snippet = options.highlightSnippet;
                let matchPos = -1;
                if (typeof options.startMs === 'number') {
                    let pos = 0;
                    while ((pos = formattedSlice.indexOf(snippet, pos)) !== -1) {
                        const chunk = formattedSlice.substring(Math.max(0, pos - 150), Math.min(formattedSlice.length, pos + 250));
                        if (chunk.includes(`"start": ${options.startMs}`)) {
                            matchPos = pos;
                            break;
                        }
                        pos += snippet.length;
                    }
                }
                if (matchPos === -1) {
                    matchPos = formattedSlice.indexOf(snippet);
                }
                if (matchPos !== -1 && typeof window.jumpToJsonMatch === 'function') {
                    window.jumpToJsonMatch(matchPos, matchPos + snippet.length, Boolean(options.focusEditor));
                }
            }, 60);
        }
    }

    if (options.syncEngine !== false) {
        scheduleEngineSync(options.debounceMs !== undefined ? options.debounceMs : 20);
    }
}
window.updateProjectTabData = updateProjectTabData;

UI.jsonEditor.addEventListener('input', () => {
    const searchInput = UI.jsonSearchInput || document.getElementById('json-search-input');
    const kw = searchInput ? searchInput.value.trim() : '';
    if (kw) {
        currentMatches = computeSearchMatches(kw);
        const curCursor = UI.jsonEditor.selectionStart || 0;
        let idx = currentMatches.findIndex(m => curCursor >= m.start && curCursor <= m.end);
        if (idx === -1) {
            idx = currentMatches.findIndex(m => m.start >= curCursor);
        }
        if (idx === -1 && currentMatches.length > 0) idx = 0;
        currentMatchIndex = idx;
        updateSearchCountUI(kw);
    } else {
        currentMatches = [];
        currentMatchIndex = -1;
        updateSearchCountUI('');
    }
    const currentText = UI.jsonEditor.value;
    tabTextBuffers[currentJsonTab] = currentText;
    let isValidJson = false;
    let parsed = null;
    try {
        parsed = JSON.parse(currentText);
        isValidJson = true;
        tabHasError[currentJsonTab] = false;
        UI.jsonEditor.classList.remove('error');
    } catch (e) {
        isValidJson = false;
        tabHasError[currentJsonTab] = true;
        UI.jsonEditor.classList.add('error');
    }
    if (isValidJson) {
        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed) && (parsed.timeline !== undefined || parsed.config !== undefined || parsed.style !== undefined)) {
            fullProjectData.config = parsed.config || {};
            fullProjectData.template = parsed.template || parsed.templates || [];
            fullProjectData.timeline = Array.isArray(parsed.timeline) ? parsed.timeline : [];
            fullProjectData.style = Array.isArray(parsed.style) ? parsed.style : (Array.isArray(parsed.roles) ? parsed.roles : (Array.isArray(parsed.styles) ? parsed.styles : []));
            tabTextBuffers.config = JSON.stringify(fullProjectData.config, null, " ");
            tabTextBuffers.template = JSON.stringify(fullProjectData.template, null, " ");
            tabTextBuffers.timeline = JSON.stringify(fullProjectData.timeline, null, " ");
            tabTextBuffers.style = JSON.stringify(fullProjectData.style, null, " ");
        } else {
            if (currentJsonTab === 'config') {
                fullProjectData.config = parsed;
                state.config = parsed;
                recordTabHistoryStep('config', parsed);
            } else if (currentJsonTab === 'template') {
                fullProjectData.template = Array.isArray(parsed) ? parsed : [];
                if (fullProjectData.templates) fullProjectData.templates = fullProjectData.template;
                state.templates = fullProjectData.template;
                recordTabHistoryStep('template', fullProjectData.template);
            } else if (currentJsonTab === 'style') {
                fullProjectData.style = Array.isArray(parsed) ? parsed : [];
                recordTabHistoryStep('style', fullProjectData.style);
            } else if (currentJsonTab === 'timeline') {
                fullProjectData.timeline = Array.isArray(parsed) ? parsed : [];
                recordTabHistoryStep('timeline', fullProjectData.timeline);
            }
        }
        state.ram.projectJson = JSON.stringify(fullProjectData, null, " ");
        state.ram.isJsonDirty = true;
        queueSessionSnapshotSave();
        scheduleEngineSync(50);
    }
});
window.performProjectUndo = function() {
    if (!window.WasmBridge || !window.WasmBridge.isReady() || !window.WasmBridge.canUndo()) return false;
    const ok = window.WasmBridge.undo();
    if (ok) {
        if (UI.jsonEditor) {
            UI.jsonEditor.value = formatTabSlice(currentJsonTab);
            UI.jsonEditor.dispatchEvent(new Event('input'));
        }
        return true;
    }
    return false;
};

window.performProjectRedo = function() {
    if (!window.WasmBridge || !window.WasmBridge.isReady() || !window.WasmBridge.canRedo()) return false;
    const ok = window.WasmBridge.redo();
    if (ok) {
        if (UI.jsonEditor) {
            UI.jsonEditor.value = formatTabSlice(currentJsonTab);
            UI.jsonEditor.dispatchEvent(new Event('input'));
        }
        return true;
    }
    return false;
};

// Ngăn hành vi mặc định của Tab và chèn 1 khoảng trắng (1 space) thay vì 4 space
UI.jsonEditor.addEventListener('keydown', (e) => {
    if (e.ctrlKey && (e.key === 'z' || e.key === 'Z')) {
        if (!e.shiftKey) {
            if (window.performProjectUndo()) {
                e.preventDefault();
                return;
            }
        } else {
            if (window.performProjectRedo()) {
                e.preventDefault();
                return;
            }
        }
    } else if (e.ctrlKey && (e.key === 'y' || e.key === 'Y')) {
        if (window.performProjectRedo()) {
            e.preventDefault();
            return;
        }
    }
    if (e.key === 'Tab') {
        e.preventDefault();
        const start = UI.jsonEditor.selectionStart;
        const end = UI.jsonEditor.selectionEnd;
        // Chèn 1 khoảng trắng (space) trực tiếp vào DOM view buffer (an toàn khi đang thu gọn code)
        if (typeof UI.jsonEditor.setRangeText === 'function') {
            UI.jsonEditor.setRangeText(" ", start, end, 'end');
        } else {
            UI.jsonEditor.value = UI.jsonEditor.value.substring(0, start) + " " + UI.jsonEditor.value.substring(end);
            UI.jsonEditor.selectionStart = UI.jsonEditor.selectionEnd = start + 1;
        }
        // Kích hoạt sự kiện input để đồng bộ Model & render lại Live Preview
        UI.jsonEditor.dispatchEvent(new Event('input'));
    }
});
// 6. Lưu file trực tiếp bằng File System Access API (Chỉ khi ấn Save mới ghi vô RAM chuẩn và file thật)
async function ensureWritableProjectHandles() {
    if (state.configFileHandle && state.templateFileHandle && state.timelineFileHandle && state.styleFileHandle && state.lyricsFileHandle) return true;
    if (!state.dirHandle) return false;
    try {
        if (typeof state.dirHandle.queryPermission === 'function') {
            let perm = await state.dirHandle.queryPermission({ mode: 'readwrite' });
            if (perm !== 'granted' && typeof state.dirHandle.requestPermission === 'function') {
                perm = await state.dirHandle.requestPermission({ mode: 'readwrite' });
            }
            if (perm !== 'granted') return false;
        }
        if (!state.configFileHandle) {
            state.configFileHandle = await state.dirHandle.getFileHandle('config.json', { create: true });
        }
        if (!state.templateFileHandle) {
            state.templateFileHandle = await state.dirHandle.getFileHandle('template.json', { create: true });
        }
        if (!state.timelineFileHandle) {
            state.timelineFileHandle = await state.dirHandle.getFileHandle('timeline.json', { create: true });
        }
        if (!state.styleFileHandle) {
            state.styleFileHandle = await state.dirHandle.getFileHandle('style.json', { create: true });
        }
        if (!state.lyricsFileHandle) {
            state.lyricsFileHandle = await state.dirHandle.getFileHandle('lyrics.txt', { create: true });
        }
        return true;
    } catch (e) {
        console.warn('[Editor] Không thể cấp quyền ghi thư mục:', e);
        return false;
    }
}
const saveJsonDirectly = async (showNotification = true) => {
    if (window.ReloadShield) window.ReloadShield.lockSave(8000);
    try {
        tryFlushCurrentEditorToRam();
        const cleanJsonStr = JSON.stringify(fullProjectData, null, " ");
        if (UI.jsonEditor) {
            UI.jsonEditor.value = formatTabSlice(currentJsonTab);
            UI.jsonEditor.classList.remove('error');
        }
        state.ram.projectJson = cleanJsonStr;
        state.ram.isJsonDirty = false;
        await ensureWritableProjectHandles();
        let savedTargetName = state.sessionProjectName || 'phiên làm việc';
        if (state.configFileHandle) {
            const wConfig = await state.configFileHandle.createWritable();
            await wConfig.write(JSON.stringify(fullProjectData.config || {}, null, " "));
            await wConfig.close();
        }
        if (state.templateFileHandle) {
            const wTemplate = await state.templateFileHandle.createWritable();
            await wTemplate.write(JSON.stringify(fullProjectData.template || [], null, " "));
            await wTemplate.close();
        }
        if (state.timelineFileHandle) {
            const wTimeline = await state.timelineFileHandle.createWritable();
            await wTimeline.write(JSON.stringify(fullProjectData.timeline || [], null, " "));
            await wTimeline.close();
        }
        if (state.styleFileHandle) {
            const wStyle = await state.styleFileHandle.createWritable();
            await wStyle.write(JSON.stringify(fullProjectData.style || [], null, " "));
            await wStyle.close();
            if (window.ReloadShield) window.ReloadShield.lockSave(8000);
        }
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.saveProject === 'function') {
            const ctx = state.projectContext || {};
            const diskSaved = await window.StockCutDiskBridge.saveProject({
                name: state.sessionProjectName || ctx.name || 'Project',
                rootPath: ctx.rootPath || '',
                projectData: fullProjectData,
                lyricsTxt: state.ram ? state.ram.lyricsTxt : undefined,
                playheadMs: typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : 0
            });
            if (diskSaved && diskSaved.rootPath && state.projectContext) {
                state.projectContext.rootPath = diskSaved.rootPath;
            }
        }
        if (typeof persistSessionProject === 'function') {
            await persistSessionProject(state.sessionProjectName, state.ram.projectJson, state.ram.lyricsTxt, {
                isJsonDirty: false
            });
        }
        if (showNotification && UI.statusText) {
            const originalText = UI.statusText.innerText;
            UI.statusText.innerText = `Đã lưu!`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => {
                UI.statusText.innerText = originalText;
                UI.statusText.classList.remove('is-success');
            }, 2000);
        }
    } catch (err) {
        console.error(err);
        alert("Lỗi: " + err.message);
    }
};
window.saveJsonDirectly = saveJsonDirectly;
const saveLyricsDirectly = async (showNotification = true) => {
    if (window.ReloadShield) window.ReloadShield.lockSave(8000);
    try {
        const lyricsEl = document.getElementById('ai-lyrics-text');
        const lyricsStr = lyricsEl ? lyricsEl.value : state.ram.lyricsTxt;
        state.ram.lyricsTxt = lyricsStr;
        state.ram.isLyricsDirty = false;
        await ensureWritableProjectHandles();
        if (state.lyricsFileHandle) {
            const writable = await state.lyricsFileHandle.createWritable();
            await writable.write(lyricsStr);
            await writable.close();
            if (window.ReloadShield) window.ReloadShield.lockSave(8000);
        }
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.saveProject === 'function') {
            const ctx = state.projectContext || {};
            await window.StockCutDiskBridge.saveProject({
                name: state.sessionProjectName || ctx.name || 'Project',
                rootPath: ctx.rootPath || '',
                lyricsTxt: lyricsStr,
                playheadMs: typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : 0
            });
        }
        if (typeof persistSessionProject === 'function') {
            await persistSessionProject(state.sessionProjectName, state.ram.projectJson, state.ram.lyricsTxt, {
                isLyricsDirty: false
            });
        }
        if (showNotification && UI.statusText) {
            const originalText = UI.statusText.innerText;
            UI.statusText.innerText = `Đã lưu thay đổi vào lyrics.txt`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => {
                UI.statusText.innerText = originalText;
                UI.statusText.classList.remove('is-success');
            }, 2000);
        }
    } catch (err) {
        console.error("Không thể lưu lyrics.txt:", err);
    }
};
window.saveLyricsDirectly = saveLyricsDirectly;
const setupLyricsEditorEvents = () => {
    const lyricsEl = document.getElementById('ai-lyrics-text');
    if (!lyricsEl || lyricsEl._hasRamListener) return;
    lyricsEl._hasRamListener = true;
    lyricsEl.addEventListener('input', () => {
        state.ram.lyricsTxt = lyricsEl.value;
        state.ram.isLyricsDirty = true;
        queueSessionSnapshotSave();
    });
    // Giảm độ nhạy cuộn chuột (Mouse wheel dampening) cho vùng edit lyric.txt
    lyricsEl.addEventListener('wheel', (e) => {
        if (!e.deltaY) return;
        e.preventDefault();
        // Hệ số giảm độ nhạy: 0.35 giúp cuộn chậm rãi, kiểm soát từng dòng lyric chính xác
        const factor = 0.35;
        let delta = e.deltaY;
        if (e.deltaMode === 1) delta *= 20; // DOM_DELTA_LINE
        else if (e.deltaMode === 2) delta *= 200; // DOM_DELTA_PAGE
        lyricsEl.scrollTop += delta * factor;
    }, { passive: false });
};
if (document.readyState === 'loading') {
    window.addEventListener('DOMContentLoaded', setupLyricsEditorEvents);
} else {
    setupLyricsEditorEvents();
}
// Bắt sự kiện Ctrl + S: Lưu thông minh theo ngữ cảnh (Lyrics hoặc JSON hoặc cả hai)
window.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && (e.key === 's' || e.key === 'S')) {
        e.preventDefault();
        const active = document.activeElement;
        const lyricsEl = document.getElementById('ai-lyrics-text');
        if (active && active === lyricsEl) {
            saveLyricsDirectly(true);
        } else if (active && active === UI.jsonEditor) {
            saveJsonDirectly(true);
        } else {
            // Khi không focus cụ thể: lưu cả 2 nếu có file handle
            saveJsonDirectly(true);
            if (state.lyricsFileHandle || (state.ram && state.ram.isLyricsDirty)) saveLyricsDirectly(false);
        }
    }
});
// ============================================================================
// TÌM KIẾM TỪ KHÓA TRONG JSON VÀ BÔI XANH TRỰC TIẾP (ZERO FOCUS STEAL)
// ============================================================================
let currentMatches = [];
let currentMatchIndex = -1;
function computeSearchMatches(keyword) {
    const editor = UI.jsonEditor;
    if (!editor) return [];
    const text = editor.value || '';
    const lowerText = text.toLowerCase();
    const lowerKeyword = (keyword || '').toLowerCase().trim();
    const matches = [];
    if (lowerKeyword) {
        let pos = 0;
        while ((pos = lowerText.indexOf(lowerKeyword, pos)) !== -1) {
            matches.push({ start: pos, end: pos + lowerKeyword.length });
            pos += lowerKeyword.length;
        }
    }
    return matches;
}
function updateSearchCountUI(keyword) {
    const countEl = UI.jsonSearchCount || document.getElementById('json-search-count');
    if (!countEl) return;
    const lowerKeyword = (keyword || '').toLowerCase().trim();
    if (!lowerKeyword || currentMatches.length === 0) {
        countEl.textContent = lowerKeyword ? '0/0' : '';
    } else {
        countEl.textContent = `${currentMatchIndex + 1}/${currentMatches.length}`;
    }
}
function jumpToMatch(startIdx, endIdx, shouldFocus = false) {
    const editor = UI.jsonEditor;
    if (!editor) return;
    if (window.__jsonIdeController && typeof window.__jsonIdeController.revealAndSelectLogicalRange === 'function') {
        window.__jsonIdeController.revealAndSelectLogicalRange(startIdx, endIdx, shouldFocus);
        return;
    }
    if (shouldFocus) editor.focus();
    editor.setSelectionRange(startIdx, endIdx);
    const textBefore = editor.value.substring(0, startIdx);
    const lineCount = textBefore.split('\n').length;
    const computedLineHeight = parseFloat(window.getComputedStyle(editor).lineHeight) || 20;
    const visibleLines = Math.floor(editor.clientHeight / computedLineHeight) || 15;
    const targetLine = Math.max(0, lineCount - Math.floor(visibleLines / 2));
    editor.scrollTop = targetLine * computedLineHeight;
}
window.jumpToJsonMatch = jumpToMatch;
function searchKeyword(keyword, direction = 0, shouldFocus = false) {
    const editor = UI.jsonEditor;
    if (!editor) return;
    const lowerKeyword = (keyword || '').toLowerCase().trim();
    currentMatches = computeSearchMatches(lowerKeyword);
    if (currentMatches.length === 0) {
        currentMatchIndex = -1;
        updateSearchCountUI(lowerKeyword);
        if (typeof window.renderHighlightedJson === 'function') {
            window.renderHighlightedJson();
        }
        return;
    }
    if (direction === 0) {
        // Tìm mới: ưu tiên match sau con trỏ hiện tại hoặc match đầu tiên
        let curCursor = editor.selectionStart || 0;
        if (window.__jsonIdeController && typeof window.__jsonIdeController.viewOffsetToLogicalOffset === 'function') {
            curCursor = window.__jsonIdeController.viewOffsetToLogicalOffset(curCursor);
        }
        let idx = currentMatches.findIndex(m => m.start >= curCursor);
        if (idx === -1) idx = 0;
        currentMatchIndex = idx;
    } else if (direction > 0) {
        currentMatchIndex = (currentMatchIndex + 1) % currentMatches.length;
    } else {
        currentMatchIndex = (currentMatchIndex - 1 + currentMatches.length) % currentMatches.length;
    }
    updateSearchCountUI(lowerKeyword);
    // Bôi xanh tất cả ký tự trùng và đánh dấu active
    if (typeof window.renderHighlightedJson === 'function') {
        window.renderHighlightedJson();
    }
    // Cuộn tới vị trí match hiện tại (tự động mở khối đang thu gọn nếu cần)
    const m = currentMatches[currentMatchIndex];
    if (m) {
        jumpToMatch(m.start, m.end, shouldFocus);
    }
}
window.searchKeyword = searchKeyword;
// Gắn sự kiện ô tìm kiếm
const searchInput = UI.jsonSearchInput || document.getElementById('json-search-input');
const prevBtn = UI.jsonSearchPrevBtn || document.getElementById('json-search-prev-btn');
const nextBtn = UI.jsonSearchNextBtn || document.getElementById('json-search-next-btn');
if (searchInput) {
    searchInput.addEventListener('input', () => {
        searchKeyword(searchInput.value, 0, false);
    });
    searchInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            e.preventDefault();
            searchKeyword(searchInput.value, e.shiftKey ? -1 : 1, false);
        } else if (e.key === 'Escape') {
            searchInput.value = '';
            searchKeyword('', 0, false);
            UI.jsonEditor.focus();
        }
    });
}
if (prevBtn) {
    prevBtn.addEventListener('click', () => {
        searchKeyword(searchInput ? searchInput.value : '', -1, false);
    });
}
if (nextBtn) {
    nextBtn.addEventListener('click', () => {
        searchKeyword(searchInput ? searchInput.value : '', 1, false);
    });
}
// Phím tắt Ctrl + F / Cmd + F để focus nhanh vào ô tìm kiếm
window.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
        e.preventDefault();
        if (searchInput) {
            searchInput.focus();
            searchInput.select();
        }
    }
});
// ============================================================================
// IDE 3-COLUMN CODE EDITOR ENGINE
//   Col 1: Line Numbers Gutter (44px fixed)
//   Col 2: Code Folding Gutter (20px fixed)
//   Col 3: Syntax Highlighted Backdrop + Interactive Textarea (1fr)
//   Model-View projection preserves 100% full logical JSON even while folded.
// ============================================================================
(() => {
    const editor = (window.UI && UI.jsonEditor) || document.getElementById('json-editor');
    if (!editor || editor._hasHighlighter) return;
    editor._hasHighlighter = true;
    editor.wrap = 'off';

    let wrapper = editor.closest('.json-hl-wrapper');
    let codeCol = wrapper ? wrapper.querySelector('.json-code-col') : null;
    let backdrop = wrapper ? wrapper.querySelector('.json-hl-backdrop') : null;
    let linesCol = wrapper ? wrapper.querySelector('.json-gutter-lines') : null;
    let linesTrack = wrapper ? wrapper.querySelector('#json-gutter-lines-track') : null;
    let foldsCol = wrapper ? wrapper.querySelector('.json-gutter-folds') : null;
    let foldsTrack = wrapper ? wrapper.querySelector('#json-gutter-folds-track') : null;

    if (!wrapper) {
        wrapper = document.createElement('div');
        wrapper.className = 'json-hl-wrapper';
        wrapper.id = 'json-hl-wrapper';
        editor.parentNode.insertBefore(wrapper, editor);
    }
    if (!linesCol) {
        linesCol = document.createElement('div');
        linesCol.className = 'json-gutter-lines';
        linesCol.id = 'json-gutter-lines';
        linesCol.setAttribute('aria-hidden', 'true');
        linesTrack = document.createElement('div');
        linesTrack.className = 'json-gutter-track';
        linesTrack.id = 'json-gutter-lines-track';
        linesCol.appendChild(linesTrack);
        wrapper.prepend(linesCol);
    } else if (!linesTrack) {
        linesTrack = document.createElement('div');
        linesTrack.className = 'json-gutter-track';
        linesTrack.id = 'json-gutter-lines-track';
        linesCol.appendChild(linesTrack);
    }
    if (!foldsCol) {
        foldsCol = document.createElement('div');
        foldsCol.className = 'json-gutter-folds';
        foldsCol.id = 'json-gutter-folds';
        foldsTrack = document.createElement('div');
        foldsTrack.className = 'json-gutter-track';
        foldsTrack.id = 'json-gutter-folds-track';
        foldsCol.appendChild(foldsTrack);
        linesCol.after(foldsCol);
    } else if (!foldsTrack) {
        foldsTrack = document.createElement('div');
        foldsTrack.className = 'json-gutter-track';
        foldsTrack.id = 'json-gutter-folds-track';
        foldsCol.appendChild(foldsTrack);
    }
    if (!codeCol) {
        codeCol = document.createElement('div');
        codeCol.className = 'json-code-col';
        codeCol.id = 'json-code-col';
        foldsCol.after(codeCol);
    }
    if (!backdrop) {
        backdrop = document.createElement('pre');
        backdrop.className = 'json-hl-backdrop';
        backdrop.setAttribute('aria-hidden', 'true');
    }
    if (backdrop.parentNode !== codeCol) codeCol.appendChild(backdrop);
    if (editor.parentNode !== codeCol) codeCol.appendChild(editor);
    editor.classList.add('json-hl-textarea');

    const valueDescriptor = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value');
    const getRawDomValue = () => (valueDescriptor && valueDescriptor.get ? valueDescriptor.get.call(editor) : '');
    const setRawDomValue = (v) => {
        if (valueDescriptor && valueDescriptor.set) valueDescriptor.set.call(editor, v);
    };

    // Model-View State
    let logicalText = getRawDomValue() || '';
    let logicalLines = logicalText.split('\n');
    let foldMap = new Map();       // startLine (0-based) -> endLine (0-based)
    let visibleRows = [];          // [{ logicalLine, text, isFoldable, isCollapsed, hiddenLines, foldEnd, logicalCharStart, viewCharStart }]
    let lastProjectedViewText = logicalText;
    let activeVisibleLine = -1;

    function getActiveFoldedSet() {
        if (!tabFoldedStarts[currentJsonTab]) {
            tabFoldedStarts[currentJsonTab] = new Set();
        }
        return tabFoldedStarts[currentJsonTab];
    }

    // Fast O(N) JSON-aware bracket matcher to discover foldable multiline blocks
    function computeFoldRanges(lines) {
        const map = new Map();
        const stack = [];
        for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
            const line = lines[lineIdx];
            let inStr = false;
            let escaped = false;
            for (let c = 0; c < line.length; c++) {
                const ch = line[c];
                if (inStr) {
                    if (escaped) {
                        escaped = false;
                    } else if (ch === '\\') {
                        escaped = true;
                    } else if (ch === '"') {
                        inStr = false;
                    }
                    continue;
                }
                if (ch === '"') {
                    inStr = true;
                } else if (ch === '{' || ch === '[') {
                    stack.push({ line: lineIdx, ch });
                } else if (ch === '}' || ch === ']') {
                    const expected = ch === '}' ? '{' : '[';
                    for (let s = stack.length - 1; s >= 0; s--) {
                        if (stack[s].ch === expected) {
                            const opened = stack[s];
                            stack.length = s;
                            // Chỉ tạo nút thu gọn cho khối có ít nhất 1 dòng nội dung ở giữa (giống VS Code)
                            if (lineIdx - opened.line >= 2) {
                                map.set(opened.line, lineIdx);
                            }
                            break;
                        }
                    }
                }
            }
        }
        return map;
    }

    function buildProjection() {
        logicalLines = logicalText.split('\n');
        foldMap = computeFoldRanges(logicalLines);
        const foldedSet = getActiveFoldedSet();
        // Loại bỏ các chỉ mục fold không còn hợp lệ sau khi chỉnh sửa
        for (const startLine of Array.from(foldedSet)) {
            if (!foldMap.has(startLine)) {
                foldedSet.delete(startLine);
            }
        }
        // Tính trước vị trí ký tự bắt đầu của từng dòng trong logicalText
        const logicalStarts = new Array(logicalLines.length);
        let charAcc = 0;
        for (let i = 0; i < logicalLines.length; i++) {
            logicalStarts[i] = charAcc;
            charAcc += logicalLines[i].length + 1;
        }

        const rows = [];
        let viewCharAcc = 0;
        for (let i = 0; i < logicalLines.length; i++) {
            const isFoldable = foldMap.has(i);
            const isCollapsed = isFoldable && foldedSet.has(i);
            const foldEnd = isFoldable ? foldMap.get(i) : -1;
            const text = logicalLines[i];
            if (isCollapsed && foldEnd > i + 1) {
                rows.push({
                    logicalLine: i,
                    text,
                    isFoldable: true,
                    isCollapsed: true,
                    hiddenLines: logicalLines.slice(i + 1, foldEnd),
                    foldEnd,
                    logicalCharStart: logicalStarts[i],
                    viewCharStart: viewCharAcc
                });
                viewCharAcc += text.length + 1;
                // Ẩn các dòng nằm giữa (i+1 .. foldEnd-1), giữ nguyên dòng đóng ngoặc foldEnd giống VS Code
                i = foldEnd - 1;
            } else {
                rows.push({
                    logicalLine: i,
                    text,
                    isFoldable,
                    isCollapsed: false,
                    hiddenLines: null,
                    foldEnd,
                    logicalCharStart: logicalStarts[i],
                    viewCharStart: viewCharAcc
                });
                viewCharAcc += text.length + 1;
            }
        }
        visibleRows = rows;
        return rows.map(r => r.text).join('\n');
    }

    // Hợp nhất chỉnh sửa trên DOM textarea (view) ngược về logicalText (model) mà không làm mất khối đang thu gọn
    function reconcileViewEditToModel() {
        const raw = getRawDomValue();
        if (raw === lastProjectedViewText) return false;
        const foldedSet = getActiveFoldedSet();
        if (foldedSet.size === 0 || visibleRows.length === 0) {
            logicalText = raw;
            const projected = buildProjection();
            lastProjectedViewText = projected;
            return true;
        }

        const oldVLines = visibleRows.map(r => r.text);
        const newVLines = raw.split('\n');
        const oldLen = oldVLines.length;
        const newLen = newVLines.length;

        let prefix = 0;
        const minLen = Math.min(oldLen, newLen);
        while (prefix < minLen && oldVLines[prefix] === newVLines[prefix]) {
            prefix++;
        }
        let suffix = 0;
        while (
            suffix < (minLen - prefix) &&
            oldVLines[oldLen - 1 - suffix] === newVLines[newLen - 1 - suffix]
        ) {
            suffix++;
        }

        const newLogicalLines = [];
        const nextFoldedSet = new Set();
        let needsDomReproject = false;

        // 1. Giữ nguyên phần prefix cùng các khối đang thu gọn
        for (let v = 0; v < prefix; v++) {
            const row = visibleRows[v];
            newLogicalLines.push(row.text);
            if (row.hiddenLines) {
                nextFoldedSet.add(newLogicalLines.length - 1);
                for (let k = 0; k < row.hiddenLines.length; k++) {
                    newLogicalLines.push(row.hiddenLines[k]);
                }
            }
        }

        // 2. Vùng thay đổi ở giữa
        const oldMidStart = prefix;
        const oldMidEnd = oldLen - suffix;
        const newMidStart = prefix;
        const newMidEnd = newLen - suffix;

        if (
            oldMidEnd - oldMidStart === 1 &&
            newMidEnd - newMidStart === 1 &&
            visibleRows[oldMidStart] &&
            visibleRows[oldMidStart].hiddenLines
        ) {
            const editedHeader = newVLines[newMidStart];
            const hidden = visibleRows[oldMidStart].hiddenLines;
            newLogicalLines.push(editedHeader);
            if (/[{\[]/.test(editedHeader)) {
                nextFoldedSet.add(newLogicalLines.length - 1);
            } else {
                needsDomReproject = true;
            }
            for (let k = 0; k < hidden.length; k++) {
                newLogicalLines.push(hidden[k]);
            }
        } else {
            const preservedHidden = [];
            for (let v = oldMidStart; v < oldMidEnd; v++) {
                if (visibleRows[v] && visibleRows[v].hiddenLines) {
                    preservedHidden.push(...visibleRows[v].hiddenLines);
                    needsDomReproject = true;
                }
            }
            for (let v = newMidStart; v < newMidEnd; v++) {
                newLogicalLines.push(newVLines[v]);
            }
            for (let k = 0; k < preservedHidden.length; k++) {
                newLogicalLines.push(preservedHidden[k]);
            }
        }

        // 3. Giữ nguyên phần suffix cùng các khối đang thu gọn (tự dịch chuyển chỉ số dòng)
        for (let v = oldLen - suffix; v < oldLen; v++) {
            const row = visibleRows[v];
            newLogicalLines.push(row.text);
            if (row.hiddenLines) {
                nextFoldedSet.add(newLogicalLines.length - 1);
                for (let k = 0; k < row.hiddenLines.length; k++) {
                    newLogicalLines.push(row.hiddenLines[k]);
                }
            }
        }

        logicalText = newLogicalLines.join('\n');
        foldedSet.clear();
        for (const s of nextFoldedSet) foldedSet.add(s);

        const prevCaretStart = editor.selectionStart;
        const prevCaretEnd = editor.selectionEnd;
        const prevScrollTop = editor.scrollTop;
        const prevScrollLeft = editor.scrollLeft;

        const projected = buildProjection();
        if (needsDomReproject || projected !== raw) {
            setRawDomValue(projected);
            lastProjectedViewText = projected;
            const clampStart = Math.min(prevCaretStart, projected.length);
            const clampEnd = Math.min(prevCaretEnd, projected.length);
            editor.setSelectionRange(clampStart, clampEnd);
            editor.scrollTop = prevScrollTop;
            editor.scrollLeft = prevScrollLeft;
        } else {
            lastProjectedViewText = raw;
        }
        return true;
    }

    const escapeHtml = (text) => text
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
    const tokenPattern = /"(?:\\.|[^"\\])*"(?=\s*:)?|\b(?:true|false|null)\b|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g;
    const colorPattern = /^(?:#[0-9a-f]{3,8}|0x[0-9a-f]{6,8}|rgba?\([^)]*\)|hsla?\([^)]*\))$/i;

    function tokenClass(token, isKey, before) {
        if (isKey) {
            if (token === '"config"') return 'hl-root-config';
            if (token === '"template"' || token === '"Template"') return 'hl-root-template';
            if (token === '"timeline"') return 'hl-root-timeline';
            return 'hl-key';
        }
        if (token.startsWith('"')) {
            const isTypeValue = /"type"\s*:\s*$/.test(before);
            if (isTypeValue) {
                let type = '';
                try {
                    type = JSON.parse(token).toLowerCase();
                } catch (error) {
                    return 'hl-string';
                }
                if (type === 'div') return 'hl-type-div hl-type-badge';
                if (type === 'effect') return 'hl-type-effect hl-type-badge';
                if (type === 'lyric' || type === 'text') return 'hl-type-lyric hl-type-badge';
                if (type === 'video' || type === 'image' || type === 'path') return 'hl-type-visual hl-type-badge';
                if (type === 'audio') return 'hl-type-audio hl-type-badge';
                if (type === 'background') return 'hl-type-bg hl-type-badge';
            }
            let value = '';
            try {
                value = JSON.parse(token);
            } catch (error) {
                return 'hl-string';
            }
            if (colorPattern.test(value)) return 'hl-color';
            return 'hl-string';
        }
        if (token === 'true' || token === 'false' || token === 'null') return 'hl-bool';
        return 'hl-number';
    }

    function computeActiveVisibleLine() {
        const pos = editor.selectionStart || 0;
        const raw = lastProjectedViewText || '';
        let line = 0;
        const limit = Math.min(pos, raw.length);
        for (let i = 0; i < limit; i++) {
            if (raw.charCodeAt(i) === 10) line++;
        }
        return line;
    }

    function updateActiveLineHighlight() {
        const nextActive = computeActiveVisibleLine();
        if (nextActive === activeVisibleLine) return;
        const prevEl = linesTrack.children[activeVisibleLine];
        if (prevEl) prevEl.classList.remove('is-active');
        const nextEl = linesTrack.children[nextActive];
        if (nextEl) nextEl.classList.add('is-active');
        activeVisibleLine = nextActive;
    }

    function syncScrollAll() {
        const st = editor.scrollTop;
        const sl = editor.scrollLeft;
        backdrop.style.transform = `translate(${-sl}px, ${-st}px)`;
        linesTrack.style.transform = `translateY(${-st}px)`;
        foldsTrack.style.transform = `translateY(${-st}px)`;
    }

    function renderHighlightedJson() {
        reconcileViewEditToModel();
        if (visibleRows.length === 0) {
            const projected = buildProjection();
            if (getRawDomValue() !== projected) {
                setRawDomValue(projected);
            }
            lastProjectedViewText = projected;
        }

        const text = logicalText;
        const sInput = UI.jsonSearchInput || document.getElementById('json-search-input');
        const kw = (sInput && sInput.value) ? sInput.value.toLowerCase().trim() : '';

        if (kw) {
            currentMatches = [];
            let p = 0;
            const lower = text.toLowerCase();
            while ((p = lower.indexOf(kw, p)) !== -1) {
                currentMatches.push({ start: p, end: p + kw.length });
                p += kw.length;
            }
        } else {
            currentMatches = [];
        }

        function renderTextWithMatches(rawSlice, logicalSliceStart, tokenCls, tokenColor) {
            if (!kw || currentMatches.length === 0) {
                const escaped = escapeHtml(rawSlice);
                if (tokenColor) return `<span class="${tokenCls}" style="--json-token-color:${tokenColor}">${escaped}</span>`;
                if (tokenCls) return `<span class="${tokenCls}">${escaped}</span>`;
                return escaped;
            }
            const sliceEnd = logicalSliceStart + rawSlice.length;
            let out = '';
            let pos = logicalSliceStart;
            for (let i = 0; i < currentMatches.length; i++) {
                const m = currentMatches[i];
                if (m.end <= logicalSliceStart) continue;
                if (m.start >= sliceEnd) break;
                const mStart = Math.max(pos, m.start);
                const mEnd = Math.min(sliceEnd, m.end);
                if (mStart > pos) {
                    const before = escapeHtml(text.slice(pos, mStart));
                    out += tokenColor
                        ? `<span class="${tokenCls}" style="--json-token-color:${tokenColor}">${before}</span>`
                        : (tokenCls ? `<span class="${tokenCls}">${before}</span>` : before);
                }
                const matchText = escapeHtml(text.slice(mStart, mEnd));
                const isActive = (i === currentMatchIndex);
                const markHtml = `<mark class="json-match-hl${isActive ? ' active' : ''}">${matchText}</mark>`;
                out += tokenColor
                    ? `<span class="${tokenCls}" style="--json-token-color:${tokenColor}">${markHtml}</span>`
                    : (tokenCls ? `<span class="${tokenCls}">${markHtml}</span>` : markHtml);
                pos = mEnd;
            }
            if (pos < sliceEnd) {
                const remaining = escapeHtml(text.slice(pos, sliceEnd));
                out += tokenColor
                    ? `<span class="${tokenCls}" style="--json-token-color:${tokenColor}">${remaining}</span>`
                    : (tokenCls ? `<span class="${tokenCls}">${remaining}</span>` : remaining);
            }
            return out;
        }

        function highlightLine(lineText, logicalLineStart) {
            let cursor = 0;
            let out = '';
            tokenPattern.lastIndex = 0;
            let match;
            while ((match = tokenPattern.exec(lineText)) !== null) {
                if (match.index > cursor) {
                    out += renderTextWithMatches(
                        lineText.slice(cursor, match.index),
                        logicalLineStart + cursor,
                        '',
                        null
                    );
                }
                const token = match[0];
                const isKey = /^\s*:/.test(lineText.slice(match.index + token.length));
                const cls = tokenClass(token, isKey, lineText.slice(Math.max(0, match.index - 32), match.index));
                let color = null;
                if (cls === 'hl-color') {
                    try {
                        const parsedVal = JSON.parse(token);
                        if (colorPattern.test(parsedVal)) {
                            color = parsedVal;
                            if (/^0x/i.test(color)) {
                                const hex = color.slice(2);
                                color = `#${hex.length === 8 ? hex.slice(2) : hex}`;
                            }
                        }
                    } catch (e) {}
                }
                out += renderTextWithMatches(token, logicalLineStart + match.index, cls, color);
                cursor = match.index + token.length;
            }
            if (cursor < lineText.length) {
                out += renderTextWithMatches(
                    lineText.slice(cursor),
                    logicalLineStart + cursor,
                    '',
                    null
                );
            }
            return out;
        }

        const curActive = computeActiveVisibleLine();
        activeVisibleLine = curActive;

        let codeHtmlParts = new Array(visibleRows.length);
        let linesHtmlParts = new Array(visibleRows.length);
        let foldsHtmlParts = new Array(visibleRows.length);

        for (let vIdx = 0; vIdx < visibleRows.length; vIdx++) {
            const row = visibleRows[vIdx];
            const activeCls = vIdx === curActive ? ' is-active' : '';
            linesHtmlParts[vIdx] = `<div class="json-ln-row${activeCls}" data-vline="${vIdx}">${row.logicalLine + 1}</div>`;

            if (row.isFoldable) {
                const collapsedCls = row.isCollapsed ? ' is-collapsed' : '';
                const label = row.isCollapsed ? 'Expand' : 'Collapse';
                foldsHtmlParts[vIdx] = `<div class="json-fold-row"><button type="button" class="json-fold-btn${collapsedCls}" data-line="${row.logicalLine}" title="${label}" aria-label="${label}"></button></div>`;
            } else {
                foldsHtmlParts[vIdx] = `<div class="json-fold-row"></div>`;
            }

            let lineHtml = highlightLine(row.text, row.logicalCharStart);
            if (row.isCollapsed) {
                lineHtml += `<span class="json-fold-ellipsis"> ...</span>`;
            }
            codeHtmlParts[vIdx] = lineHtml;
        }

        linesTrack.innerHTML = linesHtmlParts.join('');
        foldsTrack.innerHTML = foldsHtmlParts.join('');
        backdrop.innerHTML = `${codeHtmlParts.join('\n')}\n`;
        syncScrollAll();
    }
    window.renderHighlightedJson = renderHighlightedJson;

    function applyProjectionAndRender(preserveScroll = true) {
        const prevScrollTop = editor.scrollTop;
        const prevScrollLeft = editor.scrollLeft;
        const projected = buildProjection();
        setRawDomValue(projected);
        lastProjectedViewText = projected;
        renderHighlightedJson();
        if (preserveScroll) {
            editor.scrollTop = prevScrollTop;
            editor.scrollLeft = prevScrollLeft;
        }
        syncScrollAll();
    }

    function toggleFold(logicalLine) {
        reconcileViewEditToModel();
        const foldedSet = getActiveFoldedSet();
        if (!foldMap.has(logicalLine)) return;
        if (foldedSet.has(logicalLine)) {
            foldedSet.delete(logicalLine);
        } else {
            foldedSet.add(logicalLine);
        }
        applyProjectionAndRender(true);
    }

    function viewOffsetToLogicalOffset(viewOffset) {
        if (!visibleRows.length) return viewOffset;
        for (let i = 0; i < visibleRows.length; i++) {
            const row = visibleRows[i];
            const rowEnd = row.viewCharStart + row.text.length;
            if (viewOffset <= rowEnd || i === visibleRows.length - 1) {
                return row.logicalCharStart + Math.max(0, Math.min(row.text.length, viewOffset - row.viewCharStart));
            }
        }
        return viewOffset;
    }

    function revealAndSelectLogicalRange(startIdx, endIdx, shouldFocus = false) {
        reconcileViewEditToModel();
        // Tìm dòng logic chứa startIdx
        let targetLogicalLine = 0;
        let acc = 0;
        for (let i = 0; i < logicalLines.length; i++) {
            const nextAcc = acc + logicalLines[i].length + 1;
            if (startIdx < nextAcc || i === logicalLines.length - 1) {
                targetLogicalLine = i;
                break;
            }
            acc = nextAcc;
        }
        // Tự động mở các khối đang thu gọn bao quanh targetLogicalLine
        const foldedSet = getActiveFoldedSet();
        let didUnfold = false;
        for (const s of Array.from(foldedSet)) {
            const e = foldMap.get(s);
            if (e !== undefined && s < targetLogicalLine && targetLogicalLine < e) {
                foldedSet.delete(s);
                didUnfold = true;
            }
        }
        if (didUnfold) {
            applyProjectionAndRender(false);
        }

        // Chuyển đổi offset logic -> offset view trên DOM textarea
        let vStart = 0;
        let vEnd = 0;
        let targetVLine = 0;
        for (let i = 0; i < visibleRows.length; i++) {
            const row = visibleRows[i];
            const logEnd = row.logicalCharStart + row.text.length;
            if (startIdx >= row.logicalCharStart && startIdx <= logEnd) {
                vStart = row.viewCharStart + (startIdx - row.logicalCharStart);
                targetVLine = i;
            }
            if (endIdx >= row.logicalCharStart && endIdx <= logEnd) {
                vEnd = row.viewCharStart + (endIdx - row.logicalCharStart);
                break;
            }
        }
        if (vEnd < vStart) vEnd = vStart + Math.max(0, endIdx - startIdx);

        if (shouldFocus) editor.focus();
        editor.setSelectionRange(vStart, vEnd);
        const computedLineHeight = parseFloat(window.getComputedStyle(editor).lineHeight) || 20;
        const visibleLinesCount = Math.floor(editor.clientHeight / computedLineHeight) || 15;
        const targetTopLine = Math.max(0, targetVLine - Math.floor(visibleLinesCount / 2));
        editor.scrollTop = targetTopLine * computedLineHeight;
        syncScrollAll();
        updateActiveLineHighlight();
    }

    window.__jsonIdeController = {
        toggleFold,
        revealAndSelectLogicalRange,
        viewOffsetToLogicalOffset,
        getVisibleRowCount: () => visibleRows.length,
        getFoldMapSize: () => foldMap.size,
        getFoldedCount: () => getActiveFoldedSet().size
    };

    // Click nút thu gọn / mở rộng ở Cột 2
    foldsCol.addEventListener('mousedown', (e) => {
        if (e.target.closest('.json-fold-btn')) {
            e.preventDefault(); // Không làm giật focus hoặc cuộn ngoài ý muốn
        }
    });
    foldsCol.addEventListener('click', (e) => {
        const btn = e.target.closest('.json-fold-btn');
        if (!btn) return;
        const lineNum = parseInt(btn.getAttribute('data-line'), 10);
        if (!Number.isNaN(lineNum)) {
            toggleFold(lineNum);
        }
    });

    // Click vào chỉ số dòng ở Cột 1 -> chọn trọn dòng đó (giống IDE)
    linesCol.addEventListener('click', (e) => {
        const rowEl = e.target.closest('.json-ln-row');
        if (!rowEl) return;
        const vIdx = parseInt(rowEl.getAttribute('data-vline'), 10);
        if (Number.isNaN(vIdx) || !visibleRows[vIdx]) return;
        const row = visibleRows[vIdx];
        editor.focus();
        editor.setSelectionRange(row.viewCharStart, row.viewCharStart + row.text.length);
        updateActiveLineHighlight();
    });

    // Cho phép lăn chuột cuộn dọc ngay cả khi trỏ chuột đang đặt trên Cột 1 hoặc Cột 2
    const forwardGutterWheel = (e) => {
        if (!e.deltaY) return;
        e.preventDefault();
        let delta = e.deltaY;
        if (e.deltaMode === 1) delta *= 20;
        else if (e.deltaMode === 2) delta *= 200;
        editor.scrollTop += delta;
        syncScrollAll();
    };
    linesCol.addEventListener('wheel', forwardGutterWheel, { passive: false });
    foldsCol.addEventListener('wheel', forwardGutterWheel, { passive: false });

    // Đồng bộ model ngay ở capture phase trước khi các listener input khác đọc UI.jsonEditor.value
    editor.addEventListener('input', () => {
        reconcileViewEditToModel();
    }, true);
    editor.addEventListener('input', renderHighlightedJson);
    editor.addEventListener('scroll', syncScrollAll, { passive: true });
    editor.addEventListener('keyup', updateActiveLineHighlight);
    editor.addEventListener('click', updateActiveLineHighlight);
    editor.addEventListener('mouseup', updateActiveLineHighlight);
    editor.addEventListener('focus', updateActiveLineHighlight);

    if (valueDescriptor && valueDescriptor.get && valueDescriptor.set) {
        Object.defineProperty(editor, 'value', {
            configurable: true,
            get() {
                reconcileViewEditToModel();
                return logicalText;
            },
            set(value) {
                let isFullProject = false;
                const strVal = (value === null || value === undefined) ? '' : String(value);
                if (strVal) {
                    try {
                        const parsed = JSON.parse(strVal);
                        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed) && (parsed.timeline !== undefined || parsed.config !== undefined)) {
                            isFullProject = true;
                            state.ram.projectJson = strVal;
                            syncFullProjectFromRam(strVal);
                        }
                    } catch (_) {}
                }
                if (isFullProject) {
                    logicalText = tabTextBuffers[currentJsonTab] || formatTabSlice(currentJsonTab);
                } else {
                    logicalText = strVal;
                }
                const projected = buildProjection();
                setRawDomValue(projected);
                lastProjectedViewText = projected;
                renderHighlightedJson();
            }
        });
    }

    applyProjectionAndRender(false);
})();
