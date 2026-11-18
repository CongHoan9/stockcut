// ============================================================================
// PROJECT & SESSION MANAGER (NATRON / OLIVE / PIXIJS v8 ARCHITECTURE)
//   Tham chiếu kiến trúc:
//   - Natron (Natron-RB-2.6/Engine/Project.h): Canonical project environment + AutoSave/Session recovery
//   - Olive NLE (olive-master/app/node/project.h): Unified project lifecycle + CancelAtom epoch
//   - PixiJS v8 (pixijs-dev/src/assets): Centralized Asset Resolver & Deduplicated Cache
//
//   Phụ thuộc : ui.js, state.js, session-db.js, timeline-engine.js, render-config.js, clock.js, viewport.js, preview.js, waveform.js
//   Cung cấp  : loadProjectFromHandle, loadDemoProject, loadProjectFromFileList, setupDefaultLoadBtn, persistSessionProject
// ============================================================================
// 1. Lưu snapshot phiên làm việc đầy đủ vào IndexedDB (nhớ cả loại nguồn, baseUrl, trạng thái dirty và playhead)
async function persistSessionProject(
    name,
    projectJson = state.ram.projectJson,
    lyricsTxt = state.ram.lyricsTxt,
    extra = {}
) {
    if (!projectJson) return;
    const ctx = state.projectContext || {};
    const projName = name || state.sessionProjectName || ctx.name || 'Project';
    const rootPath = extra.rootPath !== undefined ? extra.rootPath : (ctx.rootPath || '');
    const playheadMs = (extra.playheadMs !== undefined)
        ? extra.playheadMs
        : (typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : 0);
    const cleanPlayhead = Number.isFinite(playheadMs) && playheadMs >= 0 ? Math.round(playheadMs) : 0;
    if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.updateWorkspace === 'function') {
        window.StockCutDiskBridge.updateWorkspace({
            activeProjectName: projName,
            activeProjectPath: rootPath,
            playheadMs: cleanPlayhead,
            aspectRatio: (state.config && state.config.aspectRatio) || 'original'
        }).catch(() => {});
    }
    if (typeof saveSessionProject !== 'function') return;
    try {
        await saveSessionProject({
            name: projName,
            sourceType: extra.sourceType || ctx.sourceType || (state.dirHandle ? 'handle' : 'demo'),
            baseUrl: extra.baseUrl !== undefined ? extra.baseUrl : (ctx.baseUrl || ''),
            rootPath,
            projectJson,
            lyricsTxt: lyricsTxt || '',
            isJsonDirty: extra.isJsonDirty !== undefined ? !!extra.isJsonDirty : !!(state.ram && state.ram.isJsonDirty),
            isLyricsDirty: extra.isLyricsDirty !== undefined ? !!extra.isLyricsDirty : !!(state.ram && state.ram.isLyricsDirty),
            playheadMs: cleanPlayhead
        });
    } catch (error) {
        console.warn('[Project] Không thể lưu snapshot phiên làm việc:', error);
    }
}
// 2. Dọn dẹp trạng thái dự án cũ trước khi nạp dự án mới (Chuẩn Olive Clear() / Natron resetProject())
function resetActiveProjectSession(revokeBlobs = true) {
    if (window.ResourceSystem && typeof window.ResourceSystem.nextProjectEpoch === 'function') {
        window.ResourceSystem.nextProjectEpoch();
        if (revokeBlobs) {
            window.ResourceSystem.clearProjectAliases(true);
        }
    }
    stopVirtualClock();
    cancelAnimationFrame(state.animationFrameId);
    if (UI.lyricContainer) UI.lyricContainer.innerHTML = '';
    if (UI.watermark) UI.watermark.innerHTML = '';
    if (window.TimelineEngine && TimelineEngine.resetProject) {
        TimelineEngine.resetProject();
    }
    if (window.CanvasCompositor && CanvasCompositor.resetProject) {
        CanvasCompositor.resetProject();
    }
    if (window.WasmBridge && window.WasmBridge.manager && window.WasmBridge.manager.resetProjectState) {
        window.WasmBridge.manager.resetProjectState();
    }
    if (window.WasmBridge && window.WasmBridge.resetTelemetry) {
        window.WasmBridge.resetTelemetry();
    }
    if (window.DebugInspector && window.DebugInspector.reset) {
        window.DebugInspector.reset();
    }
    window.dispatchEvent(new CustomEvent('project-reset'));
}
// 3. Ánh xạ tài nguyên cho dự án có baseUrl hoặc rootPath trên ổ cứng
function registerProjectBaseUrlAssets(timelineItems, baseUrl, rootPath = '') {
    if (!Array.isArray(timelineItems) || (!baseUrl && !rootPath)) return;
    const cleanBase = (baseUrl || '').replace(/\/+$/, '');
    const cleanRoot = (rootPath || '').replace(/\\/g, '/').replace(/\/+$/, '');
    const visit = (items) => {
        if (!Array.isArray(items)) return;
        for (const it of items) {
            if (!it) continue;
            if (it.src && !it.src.startsWith('data:') && !it.src.startsWith('blob:') && !it.src.startsWith('http')) {
                let mediaUrl = it.src;
                const cleanSrc = it.src.replace(/\\/g, '/').replace(/^\/+/, '').replace(/^\.\//, '');
                if (cleanSrc.startsWith('Asset/') || cleanSrc.startsWith('asset/')) {
                    mediaUrl = '/Asset/' + cleanSrc.slice(6);
                } else if (/^[A-Za-z]:\//.test(it.src.replace(/\\/g, '/')) && window.StockCutDiskBridge) {
                    mediaUrl = window.StockCutDiskBridge.toFsUrl(it.src);
                } else if (window.resolveFileUrl) {
                    mediaUrl = window.resolveFileUrl(it.src);
                } else if (window.ResourceSystem && window.ResourceSystem.resolveUrl) {
                    mediaUrl = window.ResourceSystem.resolveUrl(it.src);
                }
                if ((!mediaUrl || mediaUrl === it.src) && !mediaUrl.startsWith('/Asset/') && !mediaUrl.startsWith('/@fs/')) {
                    if (cleanRoot && window.StockCutDiskBridge) {
                        mediaUrl = window.StockCutDiskBridge.toFsUrl(`${cleanRoot}/${cleanSrc}`);
                    } else if (cleanBase) {
                        mediaUrl = `${cleanBase}/${cleanSrc.split('/').map(encodeURIComponent).join('/')}`;
                    }
                }
                if (window.TimelineEngine && TimelineEngine.registerAssetBlob) {
                    TimelineEngine.registerAssetBlob(it.src, mediaUrl, {
                        absPath: (cleanRoot && !cleanSrc.startsWith('Asset/')) ? `${cleanRoot}/${cleanSrc}` : ''
                    });
                }
            }
            if (Array.isArray(it.children)) visit(it.children);
            if (Array.isArray(it.childrent)) visit(it.childrent);
        }
    };
    visit(timelineItems);
}
// 4. Pipeline nạp dự án hợp nhất (Unified Project Mount Pipeline - Instant Non-Blocking Compile)
async function mountProjectPayload(options) {
    const {
        name = 'Project',
        sourceType = 'demo',
        baseUrl = '',
        rootPath = '',
        dirHandle = null,
        configFileHandle = null,
        templateFileHandle = null,
        timelineFileHandle = null,
        styleFileHandle = null,
        lyricsFileHandle = null,
        arrayBuf = null,
        rawText = '',
        lyricsText = '',
        isJsonDirty = false,
        isLyricsDirty = false,
        playheadMs = 0
    } = options;
    const epoch = window.ResourceSystem ? window.ResourceSystem.currentProjectEpoch() : 0;
    state.dirHandle = dirHandle;
    state.configFileHandle = configFileHandle;
    state.templateFileHandle = templateFileHandle;
    state.timelineFileHandle = timelineFileHandle;
    state.styleFileHandle = styleFileHandle;
    state.lyricsFileHandle = lyricsFileHandle;
    state.sessionProjectName = name;
    state.projectContext = {
        name,
        sourceType,
        baseUrl,
        rootPath
    };
    state.ram.projectJson = rawText;
    state.ram.isJsonDirty = !!isJsonDirty;
    state.ram.lyricsTxt = lyricsText || '';
    state.ram.isLyricsDirty = !!isLyricsDirty;
    const aiLyricsTextarea = document.getElementById('ai-lyrics-text');
    if (aiLyricsTextarea) {
        aiLyricsTextarea.value = state.ram.lyricsTxt;
    }
    let pTimeline = [];
    let pTemplates = [];
    try {
        const parsed = JSON.parse(rawText);
        state.config = parsed.config || {};
        state.templates = parsed.template || parsed.templates || [];
        pTimeline = parsed.timeline || [];
        pTemplates = state.templates;
        // Đưa config vào C++ Core Engine quản lý tập trung
        if (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.config) {
            if (state.config && Object.keys(state.config).length > 0) {
                window.WasmBridge.config.setJson(JSON.stringify(state.config));
            }
        }
    } catch (_) {
        state.config = {};
        state.templates = [];
    }
    // Khôi phục tỉ lệ khung hình (aspect ratio) từ C++ Core hoặc config.json ngay khi mount dự án
    const nativeRatio = (window.WasmBridge && window.WasmBridge.isReady() && window.WasmBridge.config)
        ? window.WasmBridge.config.getStr('aspectRatio')
        : null;
    const initialRatio = nativeRatio || (state.config && state.config.aspectRatio ? state.config.aspectRatio : 'original');
    if (typeof window.updateAspectRatioBtn === 'function') {
        window.updateAspectRatioBtn(initialRatio);
    }
    const ratioPanel = document.getElementById('aspect-ratio-panel');
    if (ratioPanel) {
        ratioPanel.querySelectorAll('.ratio-option').forEach(o => {
            o.classList.toggle('active', o.dataset.ratio === initialRatio);
        });
    }
    buildRenderConfig();
    // Khôi phục kích thước, vị trí và mức zoom của render-viewport từ config.json
    if (typeof window.restoreViewportFromConfig === 'function') {
        window.restoreViewportFromConfig();
    } else {
        initViewportSize();
    }
    if (baseUrl || rootPath) {
        registerProjectBaseUrlAssets(pTimeline, baseUrl, rootPath);
    }
    // Compile ngay lập tức vào C++ WASM Core (không chờ mạng tải ảnh/font)
    const bufToLoad = arrayBuf || new TextEncoder().encode(rawText).buffer;
    const compiled = TimelineEngine.loadProjectBuffer(bufToLoad);
    state.timeline = compiled.timeline;
    if (window.WasmBridge && window.WasmBridge.renderer && typeof window.WasmBridge.renderer.invalidateTextCaches === 'function') {
        window.WasmBridge.renderer.invalidateTextCaches();
    }
    if (baseUrl || rootPath) {
        registerProjectBaseUrlAssets(state.timeline, baseUrl, rootPath);
    }
    if (typeof window.syncProjectTabsFromRam === 'function') {
        window.syncProjectTabsFromRam(state.ram.projectJson);
    } else if (UI.jsonEditor) {
        UI.jsonEditor.value = state.ram.projectJson;
    }
    UI.statusText.textContent = `${name}`;
    const projectTitleEl = document.getElementById('header-project-title');
    if (projectTitleEl) projectTitleEl.textContent = name;
    UI.exportBtn.disabled = false;
    const targetMs = (Number.isFinite(playheadMs) && playheadMs > 0 && (!state.durationMs || playheadMs <= state.durationMs))
        ? Math.round(playheadMs)
        : 0;
    const vc = typeof ensureVirtualClock === 'function' ? ensureVirtualClock() : null;
    if (vc) vc.baseMs = targetMs;
    if (window.WasmBridge && window.WasmBridge.clock) {
        window.WasmBridge.clock.seek(targetMs);
    }
    resetWaveform();
    if (typeof initVirtualPlayerUI === 'function') initVirtualPlayerUI();
    updateFrame(targetMs);
    UI.timeDisplay.textContent = `${targetMs} ms`;
    persistSessionProject(name, rawText, lyricsText, {
        sourceType,
        baseUrl,
        rootPath,
        isJsonDirty,
        isLyricsDirty,
        playheadMs: targetMs
    });
    window.dispatchEvent(new CustomEvent('project-loaded', {
        detail: { name, sourceType, rootPath, dirHandle }
    }));
    // Tiền nạp font nhị phân và tài nguyên media bất đồng bộ có bảo vệ Epoch (Olive CancelAtom)
    const fontPromise = (window.TimelineEngine && typeof window.TimelineEngine.preloadProjectFonts === 'function')
        ? window.TimelineEngine.preloadProjectFonts(pTimeline, pTemplates, false)
        : Promise.resolve([]);
    const assetPromise = (window.TimelineEngine && typeof window.TimelineEngine.preloadAssets === 'function')
        ? window.TimelineEngine.preloadAssets(dirHandle)
        : Promise.resolve([]);
    Promise.all([fontPromise, assetPromise]).then(([, missing]) => {
        if (epoch && window.ResourceSystem && window.ResourceSystem.currentProjectEpoch() !== epoch) {
            return;
        }
        if (missing && missing.length) {
            UI.statusText.textContent = `Cảnh báo: thiếu file ${missing.join(', ')}`;
        }
        if (window.WasmBridge && window.WasmBridge.renderer && typeof window.WasmBridge.renderer.invalidateTextCaches === 'function') {
            window.WasmBridge.renderer.invalidateTextCaches();
        }
        if (window.CanvasCompositor && CanvasCompositor.invalidateScene) {
            CanvasCompositor.invalidateScene('PRELOAD_COMPLETE');
        }
        refreshTimelineWaveform();
        const curMs = typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : targetMs;
        updateFrame(curMs);
    }).catch((err) => {
        console.warn('[Project] Lỗi preloadAssets:', err);
    });
    setupDefaultLoadBtn();
}
// 5. Mở thư mục bài hát từ DirectoryHandle của File System Access API
async function loadProjectFromHandle(dirHandle, restoreOptions = null) {
    let rawText = "";
    try {
        if (window.WasmBridge && window.WasmBridge.whenReady) {
            await window.WasmBridge.whenReady();
        }
        resetActiveProjectSession(true);
        try {
            await saveSessionHandle(dirHandle);
        } catch (sessionError) {
            console.warn('[Project] Không thể lưu handle phiên làm việc:', sessionError);
        }
        const configHandle = await dirHandle.getFileHandle('config.json', { create: true });
        const templateHandle = await dirHandle.getFileHandle('template.json', { create: true });
        const timelineHandle = await dirHandle.getFileHandle('timeline.json', { create: true });
        let styleHandle = null;
        try {
            styleHandle = await dirHandle.getFileHandle('style.json', { create: true });
        } catch (_) {}
        const configFile = await configHandle.getFile();
        const templateFile = await templateHandle.getFile();
        const timelineFile = await timelineHandle.getFile();
        let styleFile = null;
        if (styleHandle) {
            try { styleFile = await styleHandle.getFile(); } catch (_) {}
        }
        let cfgObj = { aspectRatio: "original" };
        try {
            const txt = await configFile.text();
            if (txt.trim()) {
                cfgObj = JSON.parse(txt);
                if (cfgObj && cfgObj.lyricSize !== undefined) delete cfgObj.lyricSize;
            } else {
                const w = await configHandle.createWritable();
                await w.write(JSON.stringify(cfgObj, null, " "));
                await w.close();
            }
        } catch (_) {}
        let tmplObj = [];
        try {
            const txt = await templateFile.text();
            if (txt.trim()) tmplObj = JSON.parse(txt);
            else {
                const w = await templateHandle.createWritable();
                await w.write(JSON.stringify(tmplObj, null, " "));
                await w.close();
            }
        } catch (_) {}
        let tlObj = [];
        try {
            const txt = await timelineFile.text();
            if (txt.trim()) tlObj = JSON.parse(txt);
            else {
                const w = await timelineHandle.createWritable();
                await w.write(JSON.stringify(tlObj, null, " "));
                await w.close();
            }
        } catch (_) {}
        let styleObj = [];
        if (styleFile) {
            try {
                const txt = await styleFile.text();
                if (txt.trim()) styleObj = JSON.parse(txt);
                else if (styleHandle) {
                    const w = await styleHandle.createWritable();
                    await w.write(JSON.stringify(styleObj, null, " "));
                    await w.close();
                }
            } catch (_) {}
        }
        const projectData = { config: cfgObj, template: tmplObj, timeline: tlObj, style: styleObj };
        rawText = JSON.stringify(projectData, null, " ");
        let arrayBuf = new TextEncoder().encode(rawText).buffer;
        let isJsonDirty = false;
        if (restoreOptions && restoreOptions.isJsonDirty && typeof restoreOptions.projectJson === 'string') {
            rawText = restoreOptions.projectJson;
            arrayBuf = new TextEncoder().encode(rawText).buffer;
            isJsonDirty = true;
        }
        let lyricsHandle = null;
        let lyricsText = "";
        let isLyricsDirty = false;
        try {
            lyricsHandle = await dirHandle.getFileHandle('lyrics.txt', { create: true });
            lyricsText = await (await lyricsHandle.getFile()).text();
            if (!lyricsText.trim()) {
                lyricsText = "Oh ooh\nwhoa\noh whoa\n";
                const w = await lyricsHandle.createWritable();
                await w.write(lyricsText);
                await w.close();
            }
        } catch (errLyrics) {
            console.warn('[Project] Không thể nạp hoặc tạo lyrics.txt:', errLyrics);
        }
        if (restoreOptions && restoreOptions.isLyricsDirty && typeof restoreOptions.lyricsTxt === 'string') {
            lyricsText = restoreOptions.lyricsTxt;
            isLyricsDirty = true;
        }
        await mountProjectPayload({
            name: dirHandle.name,
            sourceType: 'handle',
            baseUrl: '',
            dirHandle,
            configFileHandle: configHandle,
            templateFileHandle: templateHandle,
            timelineFileHandle: timelineHandle,
            styleFileHandle: styleHandle,
            lyricsFileHandle: lyricsHandle,
            arrayBuf,
            rawText,
            lyricsText,
            isJsonDirty,
            isLyricsDirty,
            playheadMs: (restoreOptions && restoreOptions.playheadMs) || 0
        });
    } catch (error) {
        UI.statusText.textContent = `Lỗi: ${error.message}`;
        if (rawText) UI.jsonEditor.value = rawText;
        console.error('[Project] Lỗi loadProjectFromHandle:', error);
    }
}
// 6. Mở thư mục từ FileList (<input type="file" webkitdirectory>) theo chuẩn 3 file
async function loadProjectFromFileList(fileList, restoreOptions = null) {
    if (!fileList || fileList.length === 0) return;
    const files = Array.from(fileList);
    const configFile = files.find(f => f.name.toLowerCase() === 'config.json');
    const templateFile = files.find(f => f.name.toLowerCase() === 'template.json');
    const timelineFile = files.find(f => f.name.toLowerCase() === 'timeline.json');
    const styleFile = files.find(f => f.name.toLowerCase() === 'style.json');
    if (!configFile || !timelineFile) {
        alert('Thư mục dự án bắt buộc phải có config.json và timeline.json!');
        return;
    }
    try {
        if (window.WasmBridge && window.WasmBridge.whenReady) {
            await window.WasmBridge.whenReady();
        }
        resetActiveProjectSession(true);
        let cfgObj = { aspectRatio: "original" };
        try {
            cfgObj = JSON.parse(await configFile.text());
            if (cfgObj && cfgObj.lyricSize !== undefined) delete cfgObj.lyricSize;
        } catch (_) {}
        let tmplObj = [];
        if (templateFile) {
            try { tmplObj = JSON.parse(await templateFile.text()); } catch (_) {}
        }
        let tlObj = [];
        try { tlObj = JSON.parse(await timelineFile.text()); } catch (_) {}
        let styleObj = [];
        if (styleFile) {
            try { styleObj = JSON.parse(await styleFile.text()); } catch (_) {}
        }
        const projectData = { config: cfgObj, template: tmplObj, timeline: tlObj, style: styleObj };
        const rawText = JSON.stringify(projectData, null, " ");
        const arrayBuf = new TextEncoder().encode(rawText).buffer;
        const lyricsFile = files.find(f => f.name.toLowerCase() === 'lyrics.txt');
        let lyricsText = "";
        if (lyricsFile) {
            lyricsText = await lyricsFile.text();
        }
        const folderName = (restoreOptions && restoreOptions.name) ||
            (configFile.webkitRelativePath ? configFile.webkitRelativePath.split('/')[0] : 'Project');
        let resolvedRootPath = (restoreOptions && restoreOptions.rootPath) || '';
        if (!resolvedRootPath && window.StockCutDiskBridge) {
            const cfgAbs = await window.StockCutDiskBridge.resolveFilePath(configFile.name, configFile.size || 0);
            if (cfgAbs) {
                resolvedRootPath = cfgAbs.replace(/\\/g, '/').replace(/\/[^/]+$/, '');
            }
        }
        for (const f of files) {
            const lower = f.name.toLowerCase();
            if (['config.json', 'template.json', 'timeline.json', 'style.json', 'lyrics.txt'].includes(lower)) continue;
            let rel = f.name;
            if (f.webkitRelativePath) {
                const parts = f.webkitRelativePath.split('/');
                if (parts.length > 1) parts.shift();
                rel = parts.join('/');
            }
            let diskStreamUrl = '';
            let diskAbsPath = '';
            if (window.StockCutDiskBridge) {
                if (resolvedRootPath) {
                    diskAbsPath = `${resolvedRootPath}/${rel}`;
                } else {
                    diskAbsPath = await window.StockCutDiskBridge.resolveFilePath(f.name, f.size || 0);
                }
                if (diskAbsPath) {
                    diskStreamUrl = window.StockCutDiskBridge.toFsUrl(diskAbsPath);
                    window.StockCutDiskBridge.addLink({
                        absPath: diskAbsPath,
                        name: f.name,
                        size: f.size || 0
                    }).catch(() => {});
                }
            }
            if (window.TimelineEngine && TimelineEngine.registerAssetBlob) {
                TimelineEngine.registerAssetBlob(rel, diskStreamUrl || f, {
                    file: diskStreamUrl ? null : f,
                    blob: diskStreamUrl ? null : f,
                    absPath: diskAbsPath || '',
                    relPath: rel,
                    name: f.name,
                    byteLength: f.size || 0,
                    lastModified: f.lastModified || 0
                });
            }
        }
        if (window.StockCutDiskBridge && resolvedRootPath) {
            window.StockCutDiskBridge.saveProject({
                name: folderName,
                rootPath: resolvedRootPath,
                projectData,
                lyricsTxt: lyricsText,
                playheadMs: (restoreOptions && restoreOptions.playheadMs) || 0
            }).catch(() => {});
        }
        if (typeof clearSessionHandle === 'function' && (!restoreOptions || !restoreOptions.keepHandle)) {
            clearSessionHandle().catch(() => {});
        }
        await mountProjectPayload({
            name: folderName,
            sourceType: (restoreOptions && restoreOptions.sourceType) || 'filelist',
            baseUrl: (restoreOptions && restoreOptions.baseUrl) || '',
            rootPath: resolvedRootPath,
            dirHandle: (restoreOptions && restoreOptions.dirHandle) || null,
            arrayBuf,
            rawText,
            lyricsText,
            isJsonDirty: !!(restoreOptions && restoreOptions.isJsonDirty),
            isLyricsDirty: !!(restoreOptions && restoreOptions.isLyricsDirty),
            playheadMs: (restoreOptions && restoreOptions.playheadMs) || 0
        });
    } catch (err) {
        UI.statusText.textContent = `Lỗi: ${err.message}`;
        console.error('[Project] Lỗi loadProjectFromFileList:', err);
    }
}
// 7. Nạp dự án trực tiếp từ payload ổ cứng ~/.stockcut (Zero Extra Network Round-Trips)
async function loadProjectFromDiskPayload(diskPayload, restoreOptions = null) {
    if (!diskPayload || !diskPayload.timeline) return false;
    try {
        resetActiveProjectSession(true);
        const projectName = diskPayload.name || 'Project';
        const rootPath = (diskPayload.rootPath || '').replace(/\\/g, '/');
        const baseUrl = `src/${encodeURIComponent(projectName)}`;
        const cfgObj = diskPayload.config || { aspectRatio: "original" };
        if (cfgObj && cfgObj.lyricSize !== undefined) delete cfgObj.lyricSize;
        const projectData = {
            config: cfgObj,
            template: Array.isArray(diskPayload.template) ? diskPayload.template : [],
            timeline: Array.isArray(diskPayload.timeline) ? diskPayload.timeline : [],
            style: Array.isArray(diskPayload.style) ? diskPayload.style : []
        };
        let rawText = JSON.stringify(projectData, null, " ");
        let isJsonDirty = false;
        if (restoreOptions && restoreOptions.isJsonDirty && typeof restoreOptions.projectJson === 'string') {
            rawText = restoreOptions.projectJson;
            isJsonDirty = true;
        }
        let lyricsText = typeof diskPayload.lyricsText === 'string' ? diskPayload.lyricsText : '';
        let isLyricsDirty = false;
        if (restoreOptions && restoreOptions.isLyricsDirty && typeof restoreOptions.lyricsTxt === 'string') {
            lyricsText = restoreOptions.lyricsTxt;
            isLyricsDirty = true;
        }
        const arrayBuf = new TextEncoder().encode(rawText).buffer;
        await mountProjectPayload({
            name: projectName,
            sourceType: 'disk',
            baseUrl,
            rootPath,
            dirHandle: null,
            arrayBuf,
            rawText,
            lyricsText,
            isJsonDirty,
            isLyricsDirty,
            playheadMs: (restoreOptions && restoreOptions.playheadMs) || 0
        });
        return true;
    } catch (err) {
        console.warn('[Project] Không thể nạp từ diskPayload:', err);
        return false;
    }
}
// 7b. Khôi phục phiên làm việc từ Snapshot nhẹ (Zero-Blob IndexedDB Fallback)
async function loadProjectFromSnapshot(snapshot, pendingDirHandle = null) {
    if (!snapshot || typeof snapshot.projectJson !== 'string') return false;
    try {
        const parsed = JSON.parse(snapshot.projectJson);
        if (!Array.isArray(parsed.timeline)) return false;
    } catch (_) {
        return false;
    }
    const snapshotName = snapshot.name || 'Project';
    const sourceType = snapshot.sourceType || 'demo';
    const baseUrl = snapshot.baseUrl || (sourceType === 'demo' ? `src/${encodeURIComponent(snapshotName)}` : '');
    const rootPath = snapshot.rootPath || '';
    try {
        resetActiveProjectSession(true);
        const arrayBuf = new TextEncoder().encode(snapshot.projectJson).buffer;
        await mountProjectPayload({
            name: snapshotName,
            sourceType,
            baseUrl,
            rootPath,
            dirHandle: pendingDirHandle || null,
            arrayBuf,
            rawText: snapshot.projectJson,
            lyricsText: snapshot.lyricsTxt || '',
            isJsonDirty: !!snapshot.isJsonDirty,
            isLyricsDirty: !!snapshot.isLyricsDirty,
            playheadMs: snapshot.playheadMs || 0
        });
        return true;
    } catch (err) {
        console.warn('[Project] Không thể khôi phục từ snapshot:', err);
        return false;
    }
}
// 8. Nạp dự án từ thư mục src/ hoặc đường dẫn tuyệt đối trong ~/.stockcut/projects.json
async function loadDemoProject(projectName = 'Hotel Room', restoreOptions = null) {
    try {
        if (window.WasmBridge && window.WasmBridge.whenReady) {
            await window.WasmBridge.whenReady();
        }
        // Ưu tiên nạp 1 chạm trực tiếp từ ổ cứng qua StockCutDiskBridge (1 request gộp cả 5 file)
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.loadProject === 'function') {
            const diskProj = await window.StockCutDiskBridge.loadProject(projectName);
            if (diskProj && await loadProjectFromDiskPayload(diskProj, restoreOptions)) {
                return;
            }
        }
        resetActiveProjectSession(true);
        const baseUrl = `src/${encodeURIComponent(projectName)}`;
        const fetchJson = async (fileName) => {
            for (const p of [`${baseUrl}/${fileName}`, `/${baseUrl}/${fileName}`]) {
                try {
                    const res = await fetch(p);
                    if (res && res.ok) return await res.json();
                } catch (_) {}
            }
            return null;
        };
        const [cfgObj, tmplObj, tlObj, styleObj] = await Promise.all([
            fetchJson('config.json'),
            fetchJson('template.json'),
            fetchJson('timeline.json'),
            fetchJson('style.json')
        ]);
        if (!cfgObj || !tlObj) {
            throw new Error(`Không thể nạp đủ các file cấu trúc cho dự án ${projectName}`);
        }
        if (cfgObj && cfgObj.lyricSize !== undefined) delete cfgObj.lyricSize;
        const projectData = {
            config: cfgObj || { aspectRatio: "original" },
            template: tmplObj || [],
            timeline: Array.isArray(tlObj) ? tlObj : [],
            style: Array.isArray(styleObj) ? styleObj : []
        };
        let rawText = JSON.stringify(projectData, null, " ");
        let arrayBuf = new TextEncoder().encode(rawText).buffer;
        let isJsonDirty = false;
        if (restoreOptions && restoreOptions.isJsonDirty && typeof restoreOptions.projectJson === 'string') {
            rawText = restoreOptions.projectJson;
            arrayBuf = new TextEncoder().encode(rawText).buffer;
            isJsonDirty = true;
        }
        let lyricsText = "";
        for (const lPath of [
            `${baseUrl}/lyrics.txt`,
            `/${baseUrl}/lyrics.txt`
        ]) {
            try {
                const lRes = await fetch(lPath);
                if (lRes && lRes.ok) {
                    lyricsText = await lRes.text();
                    break;
                }
            } catch (_) {}
        }
        let isLyricsDirty = false;
        if (restoreOptions && restoreOptions.isLyricsDirty && typeof restoreOptions.lyricsTxt === 'string') {
            lyricsText = restoreOptions.lyricsTxt;
            isLyricsDirty = true;
        }
        await mountProjectPayload({
            name: projectName,
            sourceType: 'demo',
            baseUrl,
            rootPath: '',
            dirHandle: null,
            arrayBuf,
            rawText,
            lyricsText,
            isJsonDirty,
            isLyricsDirty,
            playheadMs: (restoreOptions && restoreOptions.playheadMs) || 0
        });
    } catch (err) {
        console.warn('[Project] Không thể tự động nạp dự án demo:', err);
    }
}
// 9. Click handler cho nút Open Folder (hỗ trợ showDirectoryPicker & fallback input)
async function handleOpenFolderClick() {
    try {
        if (window.showDirectoryPicker) {
            try {
                const dirHandle = await window.showDirectoryPicker();
                await loadProjectFromHandle(dirHandle);
                return;
            } catch (err) {
                if (err && err.name === 'AbortError') return;
                console.warn('[Project] showDirectoryPicker không thành công, chuyển sang fallback input:', err);
            }
        }
    } catch (e) {
        console.warn('[Project] showDirectoryPicker ngoại lệ:', e);
    }
    const fallbackInput = document.getElementById('fallback-folder-input');
    if (fallbackInput) {
        fallbackInput.value = '';
        fallbackInput.click();
    }
}
function setupDefaultLoadBtn() {
    if (!UI.loadBtn) return;
    UI.loadBtn.textContent = "Open Folder";
    UI.loadBtn.onclick = handleOpenFolderClick;
}
function setupFallbackFolderInput() {
    const input = document.getElementById('fallback-folder-input');
    if (input) {
        input.onchange = (e) => {
            if (e.target.files && e.target.files.length > 0) {
                loadProjectFromFileList(e.target.files);
            }
        };
    }
}
// 10. Khởi tạo phiên làm việc song song (Parallel Streaming WASM + ~/.stockcut Single-Trip Bootstrap)
async function initProjectSession() {
    setupDefaultLoadBtn();
    setupFallbackFolderInput();
    const wasmPromise = (window.WasmBridge && window.WasmBridge.whenReady)
        ? window.WasmBridge.whenReady()
        : Promise.resolve(true);
    const bootPromise = (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.bootstrap === 'function')
        ? window.StockCutDiskBridge.bootstrap().catch(() => null)
        : Promise.resolve(null);
    const snapshotPromise = (typeof getSessionProject === 'function')
        ? getSessionProject().catch(() => null)
        : Promise.resolve(null);
    const handlePromise = (typeof getSessionHandle === 'function')
        ? getSessionHandle().catch(() => null)
        : Promise.resolve(null);
    let boot = null;
    let snapshot = null;
    let handle = null;
    try {
        const [, bootRes, snapRes, handleRes] = await Promise.all([wasmPromise, bootPromise, snapshotPromise, handlePromise]);
        boot = bootRes;
        snapshot = snapRes;
        handle = handleRes;
    } catch (e) {
        console.error('[Project] Lỗi khởi tạo WebAssembly:', e);
        UI.statusText.textContent = `Lỗi khởi tạo engine: ${e.message}`;
        return;
    }
    // Ưu tiên 1: Nạp tức thì từ ~/.stockcut/bootstrap (0 extra HTTP round-trips, đọc thẳng từ ổ cứng)
    if (boot && boot.activeProject) {
        const ws = boot.workspace || {};
        const matchSnap = (snapshot && snapshot.name === boot.activeProject.name) ? snapshot : null;
        const restoreOpts = {
            playheadMs: (matchSnap && matchSnap.playheadMs) || ws.playheadMs || 0,
            isJsonDirty: !!(matchSnap && matchSnap.isJsonDirty),
            projectJson: matchSnap ? matchSnap.projectJson : undefined,
            isLyricsDirty: !!(matchSnap && matchSnap.isLyricsDirty),
            lyricsTxt: matchSnap ? matchSnap.lyricsTxt : undefined
        };
        if (await loadProjectFromDiskPayload(boot.activeProject, restoreOpts)) {
            return;
        }
    }
    // Trường hợp 2: Phiên trước là dự án Demo (ví dụ Hotel Room, Cupid)
    if (snapshot && snapshot.sourceType === 'demo') {
        if (await loadProjectFromSnapshot(snapshot, null)) {
            return;
        }
    }
    // Trường hợp 3: Phiên trước mở từ DirectoryHandle
    if (handle) {
        let perm = 'prompt';
        try {
            if (typeof handle.queryPermission === 'function') {
                perm = await handle.queryPermission({ mode: 'readwrite' });
            }
        } catch (e) {
            console.warn('[Project] Không kiểm tra được quyền thư mục:', e);
        }
        if (perm === 'granted') {
            const matchSnap = (snapshot && snapshot.name === handle.name) ? snapshot : null;
            await loadProjectFromHandle(handle, matchSnap);
            if (state.ram && state.ram.projectJson) return;
        } else if (snapshot) {
            if (await loadProjectFromSnapshot(snapshot, handle)) {
                return;
            }
        }
    }
    // Trường hợp 4: Phiên trước mở từ FileList hoặc Snapshot độc lập
    if (snapshot && await loadProjectFromSnapshot(snapshot, null)) {
        return;
    }
    setupDefaultLoadBtn();
    if (!state.ram || !state.ram.projectJson) {
        await loadDemoProject('Hotel Room');
    }
}
// Lưu nhanh vị trí playhead khi người dùng reload / đóng tab
window.addEventListener('beforeunload', () => {
    if (state.ram && state.ram.projectJson) {
        persistSessionProject(state.sessionProjectName, state.ram.projectJson, state.ram.lyricsTxt);
    }
});
// Gắn sự kiện đồng bộ ngay lập tức
setupDefaultLoadBtn();
setupFallbackFolderInput();
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initProjectSession);
} else {
    initProjectSession();
}
// Xuất các hàm ra global
window.loadProjectFromHandle = loadProjectFromHandle;
window.loadProjectFromFileList = loadProjectFromFileList;
window.loadProjectFromDiskPayload = loadProjectFromDiskPayload;
window.loadDemoProject = loadDemoProject;
window.loadProjectFromSnapshot = loadProjectFromSnapshot;
window.persistSessionProject = persistSessionProject;
window.setupDefaultLoadBtn = setupDefaultLoadBtn;
window.handleOpenFolderClick = handleOpenFolderClick;
window.registerProjectBaseUrlAssets = registerProjectBaseUrlAssets;
