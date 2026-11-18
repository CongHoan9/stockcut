// ============================================================================
// js/media-drawer.js — Quản lý kho tài nguyên Media toàn cục (Global Asset Store)
//
// Đặc điểm kiến trúc:
//   • DÙNG CHUNG CHO MỌI PROJECT: Lưu trữ trong IndexedDB (GlobalMediaStoreDB),
//     tồn tại vĩnh viễn qua mọi phiên làm việc và không phụ thuộc project nào.
//   • LINK TUYỆT ĐỐI (Absolute src): Tất cả file lưu vào /Asset/[video, image, audio]
//     hoặc URL online (https://...), khi add vào JSON timeline sẽ có src tuyệt đối.
//   • TEXT BOX INLINE NHẬP URL: Nhập link online (https://...) và nhấn Enter.
//   • TAB NÚT TEXT: Style giống label bên export (font 500, #9ca3af, gạch chân trắng).
//   • HOVER ITEM: Không viền đổi màu, không icon +, chỉ đơn giản là dịch lên trên (translateY).
//   • THÊM FILE TIỆN LỢI: Click đúp vào vùng trống hoặc Drag & Drop thẳng vào tab.
//   • OVERLAY DRAG & DROP: Lớp phủ mờ hiển thị "insert file ABC.." khi đang kéo thả.
// ============================================================================
(function (global) {
    'use strict';
    const DB_NAME = 'GlobalMediaStoreDB';
    const DB_STORE = 'media_items';
    const DB_VERSION = 1;
    // Bộ nhớ đệm tài nguyên đang hiển thị
    const mediaStore = {
        video: [],
        image: [],
        audio: []
    };
    let activeTab = 'video';
    let previewAudio = null;
    let playingAudioRow = null;
    let activeVideoPreview = null;
    let activeVideoPreviewCard = null;
    let isDbInitialized = false;
    function stopVideoPreview() {
        if (!activeVideoPreview) return;
        activeVideoPreview.pause();
        activeVideoPreview.removeAttribute('src');
        activeVideoPreview.load();
        activeVideoPreview.style.display = 'none';
        activeVideoPreview = null;
        activeVideoPreviewCard = null;
    }
    // ========================================================================
    // 1. PERSISTENCE TRỰC TIẾP VÀO ~/.stockcut (ZERO-RAM BLOB, LINK TUYỆT ĐỐI)
    // ========================================================================
    const LOCAL_LINKS_KEY = 'stockcut_user_media_links_v1';

    function readFallbackLinks() {
        try {
            return JSON.parse(localStorage.getItem(LOCAL_LINKS_KEY) || '[]');
        } catch (_) {
            return [];
        }
    }

    function writeFallbackLinks(list) {
        try {
            localStorage.setItem(LOCAL_LINKS_KEY, JSON.stringify(list));
        } catch (_) {}
    }

    async function dbSaveItem(item) {
        if (!item || item.isAsset) return;
        const record = {
            id: item.id,
            name: item.name,
            path: item.path,
            src: item.src || item.path,
            type: item.type,
            size: item.size || 0,
            duration: item.duration || 0,
            title: item.title || item.name,
            artist: item.artist || '',
            thumb: (item.thumb && !item.thumb.startsWith('blob:')) ? item.thumb : null,
            isOnline: !!item.isOnline,
            scope: 'local',
            addedAt: item.addedAt || Date.now()
        };
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.addLinks === 'function') {
            await window.StockCutDiskBridge.addLinks(record);
        }
        const list = readFallbackLinks().filter(x => x.path !== record.path && x.id !== record.id);
        list.unshift(record);
        writeFallbackLinks(list);
    }

    async function dbGetAllItems() {
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.bootstrap === 'function') {
            const boot = await window.StockCutDiskBridge.bootstrap();
            if (boot) {
                const media = Array.isArray(boot.media) ? boot.media : [];
                const audio = Array.isArray(boot.audio) ? boot.audio : [];
                return [...media, ...audio];
            }
        }
        return readFallbackLinks();
    }

    async function dbDeleteItem(idOrPath) {
        if (!idOrPath) return;
        if (window.StockCutDiskBridge && typeof window.StockCutDiskBridge.removeLink === 'function') {
            await window.StockCutDiskBridge.removeLink(idOrPath);
        }
        const list = readFallbackLinks().filter(x => x.id !== idOrPath && x.path !== idOrPath);
        writeFallbackLinks(list);
    }
    // Đăng ký blob URL & Stream 1:1 Descriptor với TimelineEngine + C++ DemuxPipeline
    function registerAssetWithEngine(path, name, blobUrl, descriptor = null) {
        if (path && /\.(mp4|mov|m4v|webm|mkv)$/i.test(path) &&
            window.WasmBridge && window.WasmBridge.stream && typeof window.WasmBridge.stream.registerSource === 'function') {
            try {
                const desc = descriptor || { url: blobUrl || path };
                window.WasmBridge.stream.registerSource(path, desc);
            } catch (err) {
                console.warn('[MediaDrawer] Stream registerSource warning:', err);
            }
        }
        if (!window.TimelineEngine || typeof window.TimelineEngine.registerAssetBlob !== 'function' || !blobUrl) return;
        if (path) {
            window.TimelineEngine.registerAssetBlob(path, blobUrl, descriptor || undefined);
        } else if (name) {
            window.TimelineEngine.registerAssetBlob(name, blobUrl, descriptor || undefined);
        }
    }
    // Hủy đăng ký blob URL khi dọn dẹp tài nguyên project cũ
    function unregisterAssetWithEngine(path, name) {
        if (!window.TimelineEngine || typeof window.TimelineEngine.unregisterAssetBlob !== 'function') return;
        if (path) window.TimelineEngine.unregisterAssetBlob(path);
        if (name) window.TimelineEngine.unregisterAssetBlob(name);
        if (path && path.startsWith('/')) {
            window.TimelineEngine.unregisterAssetBlob(path.substring(1));
        }
        if (path && path.startsWith('/Asset/')) {
            window.TimelineEngine.unregisterAssetBlob(path.replace(/^\/Asset\//, ''));
        }
    }
    // ========================================================================
    // 2. TIỆN ÍCH METADATA & PARSER
    // ========================================================================
    function formatTime(sec) {
        if (!Number.isFinite(sec) || sec <= 0) return '--:--';
        const m = Math.floor(sec / 60);
        const s = Math.floor(sec % 60);
        return `${m}:${s < 10 ? '0' : ''}${s}`;
    }
    function parseAudioMeta(fileName) {
        const clean = String(fileName || '').replace(/\.[^/.]+$/, "").trim();
        const sepMatch = clean.match(/\s+[-–—]\s+/);
        if (sepMatch) {
            const idx = sepMatch.index;
            const artist = clean.substring(0, idx).trim();
            const title = clean.substring(idx + sepMatch[0].length).trim();
            if (artist && title) return { artist, title };
        }
        return {
            artist: "Unknown Artist",
            title: clean || "Unknown Audio"
        };
    }
    async function readId3v1(file) {
        if (!file || file.size < 128) return null;
        try {
            const slice = file.slice(file.size - 128, file.size);
            const buffer = await slice.arrayBuffer();
            const bytes = new Uint8Array(buffer);
            const tag = String.fromCharCode(bytes[0], bytes[1], bytes[2]);
            if (tag === 'TAG') {
                const dec = new TextDecoder('utf-8');
                const title = dec.decode(bytes.subarray(3, 33)).replace(/\0+$/, '').trim();
                const artist = dec.decode(bytes.subarray(33, 63)).replace(/\0+$/, '').trim();
                if (title) return { title, artist: artist || "Unknown Artist" };
            }
        } catch (e) {}
        return null;
    }
    function getAudioDuration(url) {
        return new Promise((resolve) => {
            const a = new Audio();
            a.preload = 'metadata';
            let done = false;
            const finish = (d) => {
                if (done) return;
                done = true;
                resolve(Number.isFinite(d) && d > 0 ? d : 0);
            };
            a.onloadedmetadata = () => finish(a.duration);
            a.onerror = () => finish(0);
            setTimeout(() => finish(0), 2000);
            a.src = url;
        });
    }
    function getVideoMeta(url) {
        return new Promise((resolve) => {
            const v = document.createElement('video');
            v.preload = 'metadata';
            v.muted = true;
            v.playsInline = true;
            if (url && (url.startsWith('http://') || url.startsWith('https://'))) {
                try {
                    if (new URL(url).origin !== window.location.origin) {
                        v.crossOrigin = 'anonymous';
                    }
                } catch (e) {}
            }
            let done = false;
            const finish = (meta) => {
                if (done) return;
                done = true;
                resolve(meta);
            };
            v.onloadedmetadata = () => {
                const dur = v.duration;
                v.currentTime = Math.min(1.0, dur * 0.2);
            };
            v.onseeked = () => {
                try {
                    const cv = document.createElement('canvas');
                    cv.width = 160;
                    cv.height = 90;
                    const ctx = cv.getContext('2d');
                    ctx.drawImage(v, 0, 0, cv.width, cv.height);
                    finish({ duration: v.duration || 0, thumb: cv.toDataURL('image/jpeg', 0.7) });
                } catch (e) {
                    finish({ duration: v.duration || 0, thumb: null });
                }
            };
            v.onerror = () => finish({ duration: 0, thumb: null });
            setTimeout(() => finish({ duration: 0, thumb: null }), 2500);
            v.src = url;
        });
    }
    // Tiện ích cập nhật DOM tại chỗ mà không phải vẽ lại cả tab (tránh giật lag)
    function findElementByPath(selector, path) {
        if (!path) return null;
        const items = document.querySelectorAll(selector);
        for (let i = 0; i < items.length; i++) {
            if (items[i].dataset && items[i].dataset.path === path) return items[i];
        }
        return null;
    }
    function updateVideoCard(item) {
        const card = findElementByPath('.media-card', item.path);
        if (!card) return;
        const thumbWrap = card.querySelector('.media-card-thumb-wrap');
        if (!thumbWrap) return;
        if (item.thumb) {
            const img = thumbWrap.querySelector('.media-card-thumb-img');
            const icon = thumbWrap.querySelector('.media-placeholder-icon');
            if (img) {
                const resolvedThumb = (window.resolveFileUrl ? window.resolveFileUrl(item.thumb) : item.thumb) || item.thumb;
                img.src = resolvedThumb;
                img.style.display = 'block';
            }
            if (icon) {
                icon.style.display = 'none';
            }
        }
        if (item.duration) {
            const badge = thumbWrap.querySelector('.media-card-badge');
            if (badge) {
                badge.innerText = formatTime(item.duration);
                badge.style.display = 'block';
            }
            card.title = `${item.name} (${formatTime(item.duration)})\nClick to add to timeline`;
        }
    }
    function updateAudioRow(item) {
        const row = findElementByPath('.media-audio-item', item.path);
        if (!row) return;
        if (item.duration) {
            const durEl = row.querySelector('.media-audio-col-duration');
            if (durEl) durEl.innerText = formatTime(item.duration);
        }
        if (item.artist && item.artist !== 'Unknown Artist') {
            const artistEl = row.querySelector('.media-audio-col-artist');
            if (artistEl) {
                artistEl.innerText = item.artist;
                artistEl.title = `Tác giả: ${item.artist}`;
            }
        }
    }
    // Add trực tiếp từng item vào tab ngay khi load được, không đợi load xong toàn bộ
    function appendSingleItemToCurrentTab(item, prepend = false) {
        if (!item || item.type !== activeTab) return;
        if (activeTab === 'video') {
            const grid = document.getElementById('media-video-grid');
            const empty = document.getElementById('media-video-empty');
            if (!grid) return;
            if (empty) empty.style.display = 'none';
            if (findElementByPath('.media-card', item.path)) return;
            const card = createVideoCard(item);
            if (prepend && grid.firstChild) {
                grid.insertBefore(card, grid.firstChild);
            } else {
                grid.appendChild(card);
            }
            renderedCounts.video++;
        } else if (activeTab === 'image') {
            const grid = document.getElementById('media-image-grid');
            const empty = document.getElementById('media-image-empty');
            if (!grid) return;
            if (empty) empty.style.display = 'none';
            if (findElementByPath('.media-card', item.path)) return;
            const card = createImageCard(item);
            if (prepend && grid.firstChild) {
                grid.insertBefore(card, grid.firstChild);
            } else {
                grid.appendChild(card);
            }
            renderedCounts.image++;
        } else if (activeTab === 'audio') {
            const listEl = document.getElementById('media-audio-list');
            const empty = document.getElementById('media-audio-empty');
            if (!listEl) return;
            if (empty) empty.style.display = 'none';
            if (findElementByPath('.media-audio-item', item.path)) return;
            const row = createAudioRow(item);
            if (prepend && listEl.firstChild) {
                listEl.insertBefore(row, listEl.firstChild);
            } else {
                listEl.appendChild(row);
            }
            renderedCounts.audio++;
        }
    }
    // ========================================================================
    // 3. KHỞI TẠO VÀ ĐỒNG BỘ DỮ LIỆU TOÀN CỤC (CHỈ LOAD 1 LẦN DUY NHẤT)
    // ========================================================================
    const ASSET_EXTENSIONS = {
        video: new Set(['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v']),
        image: new Set(['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp']),
        audio: new Set(['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'])
    };
    async function scanAssetDirectory(type, directoryUrl, rootPath) {
        let response;
        try {
            response = await fetch(directoryUrl);
        } catch (error) {
            throw new Error(`Cannot read Asset/${type}/ directory listing at ${directoryUrl}: ${error.message}`);
        }
        if (!response.ok) {
            throw new Error(`Cannot read Asset/${type}/ directory listing (${response.status}): ${directoryUrl}`);
        }
        const listing = new DOMParser().parseFromString(await response.text(), 'text/html');
        const files = [];
        const links = listing.querySelectorAll('a[href]');
        for (const link of links) {
            const href = link.getAttribute('href');
            if (!href || href === '../' || href.startsWith('?')) continue;
            const assetUrl = new URL(href, response.url || directoryUrl);
            const currentDirectory = new URL(directoryUrl);
            if (assetUrl.origin !== currentDirectory.origin ||
                !assetUrl.pathname.startsWith(currentDirectory.pathname)) continue;
            const encodedRelativePath = assetUrl.pathname.slice(currentDirectory.pathname.length);
            const relativePath = decodeURIComponent(encodedRelativePath).replace(/\/+$/, '');
            if (!relativePath || relativePath.split('/').some(part =>
                !part || part === '.' || part === '..' || part.startsWith('.'))) continue;
            if (assetUrl.pathname.endsWith('/')) {
                files.push(...await scanAssetDirectory(type, assetUrl.href, `${rootPath}${relativePath}/`));
                continue;
            }
            const extension = relativePath.split('.').pop().toLowerCase();
            if (!ASSET_EXTENSIONS[type].has(extension)) continue;
            files.push({
                name: relativePath.split('/').pop(),
                path: `/${rootPath}${relativePath}`,
                url: assetUrl.href
            });
        }
        return files;
    }
    
    // ========================================================================
    // ĐỌC THÔNG TIN & ẢNH BÌA DETAIL TRONG FILE .MP3 (ID3v2 APIC Frame)
    // ========================================================================
    async function readId3MetaAndPicture(fileOrUrl) {
        if (!fileOrUrl) return null;
        try {
            let buffer;
            if (fileOrUrl instanceof File || fileOrUrl instanceof Blob) {
                const slice = fileOrUrl.slice(0, 524288);
                buffer = await slice.arrayBuffer();
            } else if (typeof fileOrUrl === 'string') {
                const resp = await fetch(fileOrUrl, { headers: { 'Range': 'bytes=0-262143' } });
                if (!resp.ok) return null;
                buffer = await resp.arrayBuffer();
            } else {
                return null;
            }
            const bytes = new Uint8Array(buffer);
            if (bytes[0] === 0x49 && bytes[1] === 0x44 && bytes[2] === 0x33) { // 'ID3'
                const version = bytes[3];
                const tagSize = ((bytes[6] & 0x7F) << 21) | ((bytes[7] & 0x7F) << 14) | ((bytes[8] & 0x7F) << 7) | (bytes[9] & 0x7F);
                let offset = 10;
                const maxOffset = Math.min(bytes.length, 10 + tagSize);
                let title = null;
                let artist = null;
                let coverUrl = null;
                while (offset + 10 < maxOffset) {
                    const frameId = String.fromCharCode(bytes[offset], bytes[offset+1], bytes[offset+2], bytes[offset+3]);
                    let frameSize;
                    if (version === 4) {
                        frameSize = ((bytes[offset+4] & 0x7F) << 21) | ((bytes[offset+5] & 0x7F) << 14) | ((bytes[offset+6] & 0x7F) << 7) | (bytes[offset+7] & 0x7F);
                    } else {
                        frameSize = (bytes[offset+4] << 24) | (bytes[offset+5] << 16) | (bytes[offset+6] << 8) | bytes[offset+7];
                    }
                    offset += 10;
                    if (frameSize <= 0 || offset + frameSize > bytes.length) break;
                    if (frameId === 'APIC') {
                        const enc = bytes[offset];
                        let p = offset + 1;
                        let mimeEnd = p;
                        while (mimeEnd < offset + frameSize && bytes[mimeEnd] !== 0) mimeEnd++;
                        const mime = new TextDecoder('iso-8859-1').decode(bytes.subarray(p, mimeEnd)) || 'image/jpeg';
                        p = mimeEnd + 1;
                        p += 1; // picture type
                        if (enc === 1 || enc === 2) {
                            while (p + 1 < offset + frameSize && !(bytes[p] === 0 && bytes[p+1] === 0)) p += 2;
                            p += 2;
                        } else {
                            while (p < offset + frameSize && bytes[p] !== 0) p++;
                            p += 1;
                        }
                        if (p < offset + frameSize) {
                            const imgData = bytes.subarray(p, offset + frameSize);
                            const blob = new Blob([imgData], { type: mime });
                            coverUrl = URL.createObjectURL(blob);
                        }
                    } else if (frameId === 'TIT2') {
                        title = new TextDecoder('utf-8').decode(bytes.subarray(offset + 1, offset + frameSize)).replace(/\0+$/, '').trim();
                    } else if (frameId === 'TPE1') {
                        artist = new TextDecoder('utf-8').decode(bytes.subarray(offset + 1, offset + frameSize)).replace(/\0+$/, '').trim();
                    }
                    offset += frameSize;
                }
                return { title, artist, coverUrl };
            }
        } catch (e) {}
        return null;
    }
    function ensureItemMetaLazy(item) {
        if (!item || item._metaLoading || item._metaLoaded) return;
        const rawSrc = item.blobUrl || item.path;
        if (!rawSrc) return;
        const src = (window.resolveFileUrl ? window.resolveFileUrl(rawSrc) : rawSrc) || rawSrc;
        if (item.type === 'video' && (!item.duration || !item.thumb)) {
            item._metaLoading = true;
            getVideoMeta(src).then(meta => {
                item._metaLoading = false;
                item._metaLoaded = true;
                let updated = false;
                if (meta.duration && !item.duration) { item.duration = meta.duration; updated = true; }
                if (meta.thumb && !item.thumb) { item.thumb = meta.thumb; updated = true; }
                if (updated) {
                    if (!item.isAsset) dbSaveItem(item);
                    updateVideoCard(item);
                }
            }).catch(() => { item._metaLoading = false; });
        } else if (item.type === 'audio') {
            item._metaLoading = true;
            getAudioDuration(src).then(dur => {
                if (dur) item.duration = dur;
                readId3MetaAndPicture(src).then(meta => {
                    item._metaLoading = false;
                    item._metaLoaded = true;
                    if (meta) {
                        if (meta.title && (!item.title || item.title === 'Unknown Audio')) item.title = meta.title;
                        if (meta.artist && (!item.artist || item.artist === 'Unknown Artist')) item.artist = meta.artist;
                        if (meta.coverUrl && !item.thumb) item.thumb = meta.coverUrl;
                    }
                    if (!item.isAsset) dbSaveItem(item);
                    updateAudioRow(item);
                }).catch(() => {
                    item._metaLoading = false;
                    item._metaLoaded = true;
                    updateAudioRow(item);
                });
            }).catch(() => { item._metaLoading = false; });
        }
    }
    async function loadRootAssetCatalog() {
        if (window.ResourceSystem) {
            await window.ResourceSystem.loadManifest('Asset/manifest.json');
            ['video', 'image', 'audio'].forEach((type) => {
                const list = window.ResourceSystem.list(type);
                for (const asset of list) {
                    const ext = (asset.name || asset.path || '').split('.').pop().toLowerCase();
                    if (type === 'video' && ['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) continue;
                    if (type === 'video' && ['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) continue;
                    if (type === 'image' && ['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v', 'mp3', 'wav', 'm4a'].includes(ext)) continue;
                    if (type === 'audio' && ['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v', 'png', 'jpg', 'jpeg', 'webp', 'gif'].includes(ext)) continue;
                    if (mediaStore[type].some(item => item.path === asset.path || item.id === asset.id || item.name === asset.name)) continue;
                    mediaStore[type].push(asset);
                    if (type === activeTab && renderedCounts[type] < PAGE_SIZE) {
                        appendSingleItemToCurrentTab(asset, false);
                    }
                }
            });
            if (mediaStore[activeTab] && mediaStore[activeTab].length > 0 && renderedCounts[activeTab] === 0) {
                renderCurrentTab(false);
            }
            return;
        }
        // 2. Dự phòng: Quét qua thư mục HTTP nếu không nạp được manifest.json
        const baseUrl = new URL('Asset/', document.baseURI);
        const scanErrors = [];
        // Quét song song độc lập cả 3 danh mục video, image, audio — không danh sách nào chờ danh sách nào
        await Promise.all(['video', 'image', 'audio'].map(async (type) => {
            const directoryUrl = new URL(`${type}/`, baseUrl);
            let assets;
            try {
                assets = await scanAssetDirectory(type, directoryUrl.href, `Asset/${type}/`);
            } catch (error) {
                console.warn(`[MediaDrawer] Fallback scan Asset/${type}/ failed:`, error);
                scanErrors.push(`Asset/${type}/`);
                return;
            }
            for (const asset of assets) {
                if (mediaStore[type].some(item => item.path === asset.path)) continue;
                const item = {
                    id: `asset_${type}_${asset.path.replace(/[^a-zA-Z0-9]/g, '_')}`,
                    name: asset.name,
                    path: asset.path,
                    src: asset.path,
                    type,
                    size: 0,
                    duration: 0,
                    thumb: type === 'image' ? asset.url : null,
                    title: parseAudioMeta(asset.name).title,
                    artist: parseAudioMeta(asset.name).artist,
                    blobUrl: asset.url,
                    isOnline: false,
                    isAsset: true,
                    addedAt: 0
                };
                registerAssetWithEngine(item.path, item.name, item.blobUrl, { url: asset.url });
                mediaStore[type].push(item);
                // Tải lười theo chunk: chỉ append ngay vào cha nếu chưa vượt quá chunk hiện tại
                if (type === activeTab && renderedCounts[type] < PAGE_SIZE) {
                    appendSingleItemToCurrentTab(item, false);
                }
            }
        }));
        if (mediaStore[activeTab] && mediaStore[activeTab].length > 0 && renderedCounts[activeTab] === 0) {
            renderCurrentTab(false);
        }
        if (scanErrors.length && UI.statusText) {
            UI.statusText.textContent = `Không thể quét ${scanErrors.join(', ')}. Hãy chạy ứng dụng qua HTTP server cho phép liệt kê thư mục.`;
        }
    }
    async function initGlobalMediaStore(force = false) {
        if (isDbInitialized && !force) {
            return;
        }
        isDbInitialized = true;
        mediaStore.video = [];
        mediaStore.image = [];
        mediaStore.audio = [];
        // Khởi chạy song song: (1) Nạp link tuyệt đối từ ~/.stockcut và (2) Nạp danh mục cấp sẵn /Asset/
        const dbTask = (async () => {
            try {
                const savedItems = await dbGetAllItems();
                for (const item of savedItems) {
                    if (!item || !item.path) continue;
                    const ext = (item.name || item.path || '').split('.').pop().toLowerCase();
                    let targetType = item.type;
                    if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) targetType = 'audio';
                    else if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) targetType = 'image';
                    else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) targetType = 'video';
                    item.type = targetType;
                    const streamUrl = (window.resolveFileUrl ? window.resolveFileUrl(item.path) : item.path) || item.path;
                    item.blobUrl = streamUrl;
                    item.url = streamUrl;
                    if (item.type === 'image' && !item.thumb) {
                        item.thumb = streamUrl;
                    }
                    registerAssetWithEngine(item.path, item.name, streamUrl, { url: streamUrl });
                    if (mediaStore[item.type] && !mediaStore[item.type].some(x => x.path === item.path)) {
                        mediaStore[item.type].push(item);
                        if (item.type === activeTab && renderedCounts[item.type] < PAGE_SIZE) {
                            appendSingleItemToCurrentTab(item, false);
                        }
                    }
                }
            } catch (e) {
                console.warn('[MediaDrawer] Lỗi nạp store:', e);
            }
        })();
        const assetTask = loadRootAssetCatalog();
        await Promise.all([dbTask, assetTask]);
    }
    // ========================================================================
    // 4. XỬ LÝ NHẬP FILE (TRUY CẬP TRỰC TIẾP Ổ CỨNG & LƯU LINK TUYỆT ĐỐI VÀO ~/.stockcut)
    // ========================================================================
    async function importFiles(files, isUserAction = true) {
        if (!files || !files.length) return;
        let lastType = activeTab;
        let count = 0;
        for (const file of Array.from(files)) {
            const ext = file.name.split('.').pop().toLowerCase();
            let type = activeTab;
            if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) type = 'video';
            else if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) type = 'image';
            else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) type = 'audio';
            lastType = type;

            // Tìm đường dẫn tuyệt đối thật trên ổ cứng (C:/..., D:/...) qua StockCutDiskBridge
            let realDiskPath = file.path ? String(file.path).replace(/\\/g, '/') : '';
            if (!realDiskPath && window.StockCutDiskBridge && typeof window.StockCutDiskBridge.resolveLocalFile === 'function') {
                realDiskPath = await window.StockCutDiskBridge.resolveLocalFile(file.name, file.size || 0);
            }
            const absPath = realDiskPath || `/Asset/${type}/${file.name}`;
            const streamUrl = realDiskPath
                ? (window.resolveFileUrl ? window.resolveFileUrl(realDiskPath) : `/@fs/${realDiskPath}`)
                : URL.createObjectURL(file);
            if (!realDiskPath && window.registerUserFileUrl) {
                window.registerUserFileUrl(absPath, streamUrl);
            }
            registerAssetWithEngine(absPath, file.name, streamUrl, { url: streamUrl, size: file.size || 0 });
            const item = {
                id: 'media_' + Date.now() + '_' + Math.random().toString(36).slice(2, 7),
                name: file.name,
                path: absPath, // Link tuyệt đối thật trên ổ cứng người dùng!
                src: absPath,
                type,
                size: file.size,
                blobUrl: streamUrl,
                url: streamUrl,
                duration: 0,
                thumb: type === 'image' ? streamUrl : null,
                isOnline: false,
                isAsset: false,
                scope: 'local',
                addedAt: Date.now()
            };
            if (type === 'audio') {
                const parsed = parseAudioMeta(file.name);
                item.title = parsed.title;
                item.artist = parsed.artist;
                readId3v1(file).then(id3 => {
                    if (id3 && id3.title) item.title = id3.title;
                    if (id3 && id3.artist) item.artist = id3.artist;
                    dbSaveItem(item);
                    updateAudioRow(item);
                }).catch(() => {});
            }
            mediaStore[type] = mediaStore[type].filter(x => x.path !== absPath);
            mediaStore[type].unshift(item);
            await dbSaveItem(item);
            count++;
        }
        if (lastType !== activeTab && files.length === 1) {
            switchTab(lastType);
        } else {
            renderCurrentTab(true);
        }
        if (isUserAction && UI.statusText && count > 0) {
            UI.statusText.innerText = `Đã lưu ${count} link tuyệt đối vào .stockcut`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => UI.statusText.classList.remove('is-success'), 3000);
        }
    }
    // Quét đệ quy thư mục dự án để tìm file media (gọi onFileFound ngay khi tìm thấy từng FileSystemFileHandle)
    async function scanProjectFiles(dirHandle, onFileFound) {
        if (!dirHandle) return;
        try {
            for await (const [name, handle] of dirHandle.entries()) {
                if (name.startsWith('.') || name === 'node_modules' || name === '__pycache__') continue;
                if (handle.kind === 'directory') {
                    await scanProjectFiles(handle, onFileFound);
                } else if (handle.kind === 'file') {
                    const ext = name.split('.').pop().toLowerCase();
                    if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v', 'png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp', 'mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) {
                        try {
                            const f = await handle.getFile();
                            if (typeof onFileFound === 'function') {
                                await onFileFound(f, handle);
                            }
                        } catch (e) {}
                    }
                }
            }
        } catch (e) {}
    }
    // Xóa sạch toàn bộ media của project cũ khi nạp project mới
    async function clearProjectMedia() {
        for (const type of ['video', 'image', 'audio']) {
            const removedItems = [];
            mediaStore[type] = mediaStore[type].filter(item => {
                if (item.isProjectFile) {
                    removedItems.push(item);
                    return false;
                }
                return true;
            });
            for (const item of removedItems) {
                if (item.blobUrl && typeof item.blobUrl === 'string' && item.blobUrl.startsWith('blob:')) {
                    try { URL.revokeObjectURL(item.blobUrl); } catch (e) {}
                }
                unregisterAssetWithEngine(item.path, item.name);
                if (item.id) {
                    await dbDeleteItem(item.id);
                }
            }
        }
    }
    // Làm sạch DOM và reset bộ đếm lazy loading cho tất cả các tab
    function resetAndRenderAllTabs() {
        renderedCounts.video = 0;
        renderedCounts.image = 0;
        renderedCounts.audio = 0;
        const vg = document.getElementById('media-video-grid');
        if (vg) vg.innerHTML = '';
        const ig = document.getElementById('media-image-grid');
        if (ig) ig.innerHTML = '';
        const al = document.getElementById('media-audio-list');
        if (al) al.innerHTML = '';
        renderCurrentTab(true);
    }
    let currentSyncDirName = null;
    // Đồng bộ media từ thư mục dự án khi mở (Bảo toàn catalog toàn cục, chỉ thêm file mới, không xóa)
    async function syncProjectMedia(dirHandle) {
        if (!dirHandle) return;
        try {
            currentSyncDirName = dirHandle.name;
            await scanProjectFiles(dirHandle, async (file, fileHandle) => {
                const ext = file.name.split('.').pop().toLowerCase();
                let type = 'video';
                if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) type = 'image';
                else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) type = 'audio';
                else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) type = 'video';
                else return;
                const absPath = `/Asset/${type}/${file.name}`;
                const alreadyExists = mediaStore[type].some(x => x.path === absPath || x.name === file.name);
                if (alreadyExists) return;
                const blobUrl = URL.createObjectURL(file);
                registerAssetWithEngine(absPath, file.name, blobUrl, {
                    handle: fileHandle || null,
                    file,
                    url: blobUrl,
                    size: file.size
                });
                const record = {
                    id: 'proj_' + Date.now() + '_' + Math.random().toString(36).slice(2, 7),
                    name: file.name,
                    path: absPath,
                    src: absPath,
                    type,
                    size: file.size,
                    blob: file,
                    fileHandle: fileHandle || null,
                    blobUrl,
                    duration: 0,
                    thumb: type === 'image' ? blobUrl : null,
                    title: parseAudioMeta(file.name).title,
                    artist: parseAudioMeta(file.name).artist,
                    isOnline: false,
                    isProjectFile: true,
                    projectDir: dirHandle.name || 'project',
                    addedAt: Date.now()
                };
                mediaStore[type].push(record);
                dbSaveItem(record);
                // Tải lười theo chunk: chỉ append ngay vào DOM của tab đang xem nếu chưa vượt quá PAGE_SIZE
                if (type === activeTab && renderedCounts[type] < PAGE_SIZE) {
                    appendSingleItemToCurrentTab(record, false);
                }
                if (type === 'audio') {
                    // Đọc ID3v1 cực nhẹ bằng file.slice(file.size - 128, file.size) (128 bytes)
                    readId3v1(file).then(id3 => {
                        if (id3 && id3.title) record.title = id3.title;
                        if (id3 && id3.artist) record.artist = id3.artist;
                        dbSaveItem(record);
                        updateAudioRow(record);
                    }).catch(() => {});
                }
            });
        } catch (e) {
            console.warn('[MediaDrawer] syncProjectMedia error:', e);
        }
    }
    // ========================================================================
    // 5. XỬ LÝ NHẬP URL ONLINE (https://... -> src tuyệt đối)
    // ========================================================================
    async function handleOnlineUrlInput(url) {
        if (!url || typeof url !== 'string') return;
        const clean = url.trim();
        if (!clean) return;
        if (!/^(https?:|data:|\/)/i.test(clean)) {
            alert('Vui lòng nhập đường dẫn URL hợp lệ bắt đầu bằng https://, http:// hoặc /');
            return;
        }
        let type = activeTab;
        const noQuery = clean.split('?')[0].split('#')[0];
        const ext = noQuery.split('.').pop().toLowerCase();
        if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) type = 'video';
        else if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) type = 'image';
        else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac'].includes(ext)) type = 'audio';
        let name = noQuery.split('/').filter(Boolean).pop() || `media_${Date.now()}`;
        if (!name.includes('.')) name += `_${type}`;
        const item = {
            id: 'url_' + Date.now() + '_' + Math.random().toString(36).slice(2, 7),
            name,
            path: clean, // Link online tuyệt đối!
            src: clean,
            type,
            size: 0,
            duration: 0,
            thumb: type === 'image' ? clean : null,
            title: parseAudioMeta(name).title,
            artist: parseAudioMeta(name).artist,
            blobUrl: clean,
            isOnline: true,
            addedAt: Date.now()
        };
        registerAssetWithEngine(clean, name, clean);
        if (type === 'video') {
            getVideoMeta(clean).then(meta => {
                item.duration = meta.duration;
                if (meta.thumb) item.thumb = meta.thumb;
                dbSaveItem(item);
                renderCurrentTab();
            }).catch(() => {});
        } else if (type === 'audio') {
            getAudioDuration(clean).then(dur => {
                item.duration = dur;
                dbSaveItem(item);
                renderCurrentTab();
            }).catch(() => {});
        }
        mediaStore[type] = mediaStore[type].filter(x => x.path !== clean);
        mediaStore[type].unshift(item);
        await dbSaveItem(item);
        if (type !== activeTab) {
            switchTab(type);
        } else {
            renderCurrentTab();
        }
        if (UI.statusText) {
            UI.statusText.innerText = `✓ Đã thêm link media online: "${name}"`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => UI.statusText.classList.remove('is-success'), 3000);
        }
    }
    // ========================================================================
    // 6. RENDER GIAO DIỆN CÁC TAB (LAZY LOADING - NẠP THEO ĐỢT KHI CUỘN)
    // ========================================================================
    const PAGE_SIZE = 12;
    const renderedCounts = {
        video: 0,
        image: 0,
        audio: 0
    };
    function renderCurrentTab(reset = false) {
        if (activeTab === 'video') renderVideoBatch(reset);
        else if (activeTab === 'image') renderImageBatch(reset);
        else if (activeTab === 'audio') renderAudioBatch(reset);
        ensureEnoughContent();
    }
    function loadMoreCurrentTab() {
        if (activeTab === 'video') renderVideoBatch(false);
        else if (activeTab === 'image') renderImageBatch(false);
        else if (activeTab === 'audio') renderAudioBatch(false);
    }
    let scrollRafId = null;
    function onDrawerScroll(e) {
        if (scrollRafId) return;
        scrollRafId = requestAnimationFrame(() => {
            scrollRafId = null;
            const el = e.target;
            if (el.scrollHeight - (el.scrollTop + el.clientHeight) <= 180) {
                loadMoreCurrentTab();
            }
        });
    }
    function ensureEnoughContent() {
        const drawer = document.getElementById('media-drawer');
        if (!drawer || !drawer.classList.contains('is-open')) return;
        const drawerBody = document.querySelector('.media-drawer-body');
        if (!drawerBody || drawerBody.clientHeight <= 40) return;
        const list = mediaStore[activeTab] || [];
        if (renderedCounts[activeTab] < list.length && drawerBody.scrollHeight <= drawerBody.clientHeight + 40) {
            loadMoreCurrentTab();
        }
    }
    function createVideoCard(v) {
        ensureItemMetaLazy(v);
        const hasThumb = Boolean(v.thumb);
        const rawThumb = v.thumb || '';
        const thumb = rawThumb ? ((window.resolveFileUrl ? window.resolveFileUrl(rawThumb) : rawThumb) || rawThumb) : '';
        const durationText = v.duration ? formatTime(v.duration || 0) : '';
        const card = (window.Templates && typeof window.Templates.renderElement === 'function')
            ? Templates.renderElement('media-card', {
                path: v.path,
                name: v.name,
                thumb,
                hasThumb,
                durationText,
                isVideo: true
            })
            : document.createElement('div');
        if (!window.Templates) {
            card.className = 'media-card';
            card.dataset.path = v.path;
            card.title = `${v.name}${durationText ? ` (${durationText})` : ''}\nClick to add to timeline`;
            card.innerHTML = `
                <div class="media-card-thumb-wrap">
                    <div class="media-placeholder-icon" style="${hasThumb ? 'display:none;' : ''}">
                        <span class="media-placeholder-tag">VIDEO</span>
                    </div>
                    <img class="media-card-thumb-img" src="${hasThumb ? thumb : ''}" alt="${v.name}" loading="lazy" style="${hasThumb ? '' : 'display:none;'}">
                    <span class="media-card-badge" style="${durationText ? '' : 'display:none;'}">${durationText}</span>
                </div>
                <div class="media-card-info">
                    <div class="media-card-name" title="${v.name}">${v.name}</div>
                </div>
            `;
        }
        const thumbWrap = card.querySelector('.media-card-thumb-wrap');
        const rawPreview = v.blobUrl || v.path;
        const previewSrc = (window.resolveFileUrl ? window.resolveFileUrl(rawPreview) : rawPreview) || rawPreview;
        if (thumbWrap && previewSrc) {
            let previewVideo = null;
            const stopPreview = () => {
                if (activeVideoPreviewCard === card) stopVideoPreview();
            };
            const startPreview = () => {
                if (activeVideoPreviewCard === card) return;
                stopVideoPreview();
                if (!previewVideo) {
                    previewVideo = document.createElement('video');
                    previewVideo.className = 'media-card-preview';
                    previewVideo.muted = true;
                    previewVideo.playsInline = true;
                    previewVideo.loop = true;
                    previewVideo.preload = 'none';
                    const badge = thumbWrap.querySelector('.media-card-badge');
                    thumbWrap.insertBefore(previewVideo, badge || null);
                }
                previewVideo.src = previewSrc;
                previewVideo.style.display = 'block';
                activeVideoPreview = previewVideo;
                activeVideoPreviewCard = card;
                const playback = previewVideo.play();
                if (playback && typeof playback.catch === 'function') {
                    playback.catch((error) => {
                        if (activeVideoPreview === previewVideo &&
                            error.name !== 'AbortError') {
                            console.warn('[MediaDrawer] Video thumbnail preview failed:', error);
                            stopPreview();
                        }
                    });
                }
            };
            card.addEventListener('mouseenter', startPreview);
            card.addEventListener('mouseleave', stopPreview);
        }
        card.addEventListener('click', () => {
            addMediaToTimeline(v, card);
        });
        return card;
    }
    function renderVideoBatch(reset = false) {
        const grid = document.getElementById('media-video-grid');
        const empty = document.getElementById('media-video-empty');
        if (!grid) return;
        const list = mediaStore.video;
        if (!list.length) {
            stopVideoPreview();
            grid.innerHTML = '';
            if (empty) empty.style.display = 'block';
            renderedCounts.video = 0;
            return;
        }
        if (empty) empty.style.display = 'none';
        if (reset || renderedCounts.video === 0) {
            stopVideoPreview();
            grid.innerHTML = '';
            renderedCounts.video = 0;
        }
        const startIdx = renderedCounts.video;
        const endIdx = Math.min(list.length, startIdx + PAGE_SIZE);
        if (startIdx >= endIdx) return;
        let idx = startIdx;
        function renderNext() {
            if (activeTab !== 'video' || idx >= endIdx) return;
            const item = list[idx];
            if (!findElementByPath('.media-card', item.path)) {
                grid.appendChild(createVideoCard(item));
            }
            idx++;
            renderedCounts.video = idx;
            if (idx < endIdx) {
                requestAnimationFrame(renderNext);
            } else {
                ensureEnoughContent();
            }
        }
        renderNext();
    }
    function createImageCard(im) {
        const rawSrc = im.thumb || im.blobUrl || im.path;
        const src = (window.resolveFileUrl ? window.resolveFileUrl(rawSrc) : rawSrc) || rawSrc;
        const card = (window.Templates && typeof window.Templates.renderElement === 'function')
            ? Templates.renderElement('media-card', {
                path: im.path,
                name: im.name,
                src,
                isVideo: false
            })
            : document.createElement('div');
        if (!window.Templates) {
            card.className = 'media-card';
            card.dataset.path = im.path;
            card.title = `${im.name}\nClick to add to timeline`;
            card.innerHTML = `
                <div class="media-card-thumb-wrap">
                    <img src="${src}" alt="${im.name}" loading="lazy">
                </div>
                <div class="media-card-info">
                    <div class="media-card-name" title="${im.name}">${im.name}</div>
                </div>
            `;
        }
        card.addEventListener('click', () => {
            addMediaToTimeline(im, card);
        });
        return card;
    }
    function renderImageBatch(reset = false) {
        const grid = document.getElementById('media-image-grid');
        const empty = document.getElementById('media-image-empty');
        if (!grid) return;
        const list = mediaStore.image;
        if (!list.length) {
            grid.innerHTML = '';
            if (empty) empty.style.display = 'block';
            renderedCounts.image = 0;
            return;
        }
        if (empty) empty.style.display = 'none';
        if (reset || renderedCounts.image === 0) {
            grid.innerHTML = '';
            renderedCounts.image = 0;
        }
        const startIdx = renderedCounts.image;
        const endIdx = Math.min(list.length, startIdx + PAGE_SIZE);
        if (startIdx >= endIdx) return;
        let idx = startIdx;
        function renderNext() {
            if (activeTab !== 'image' || idx >= endIdx) return;
            const item = list[idx];
            if (!findElementByPath('.media-card', item.path)) {
                grid.appendChild(createImageCard(item));
            }
            idx++;
            renderedCounts.image = idx;
            if (idx < endIdx) {
                requestAnimationFrame(renderNext);
            } else {
                ensureEnoughContent();
            }
        }
        renderNext();
    }
    function createAudioRow(aud) {
        ensureItemMetaLazy(aud);
        const displayName = aud.title || (aud.name ? aud.name.replace(/\.[^/.]+$/, "") : "audio");
        const durationText = formatTime(aud.duration || 180);
        const artist = aud.artist || 'BUNE Music';
        const row = document.createElement('div');
        row.className = 'audio-track-item';
        row.dataset.path = aud.path;
        row.title = `Click to add "${displayName}" to timeline`;
        // Tạo màu gradient đại diện nếu chưa có ảnh APIC
        const gradients = [
            'linear-gradient(135deg, #1e3a8a, #06b6d4)',
            'linear-gradient(135deg, #7c2d12, #f59e0b)',
            'linear-gradient(135deg, #4c1d95, #ec4899)',
            'linear-gradient(135deg, #064e3b, #10b981)',
            'linear-gradient(135deg, #831843, #f43f5e)'
        ];
        const hash = Math.abs(String(displayName).split('').reduce((acc, c) => acc + c.charCodeAt(0), 0));
        const bgGrad = gradients[hash % gradients.length];
        const coverHtml = aud.thumb
            ? `<img src="${aud.thumb}" alt="${displayName}" class="audio-track-img">`
            : `<div class="audio-track-cover-badge" style="background: ${bgGrad};">
                 <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle></svg>
               </div>`;
        row.innerHTML = `
            <div class="audio-track-cover">${coverHtml}</div>
            <div class="audio-track-meta">
                <span class="audio-track-title">${displayName}</span>
                <span class="audio-track-sub">${durationText} · ${artist}</span>
            </div>
            <div class="audio-track-actions">
                <button type="button" class="audio-track-btn audio-play-btn" title="Nghe thử">
                    <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>
                </button>
                <button type="button" class="audio-track-btn audio-add-btn" title="Thêm vào timeline">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"></line><line x1="5" y1="12" x2="19" y2="12"></line></svg>
                </button>
            </div>
        `;
        const playBtn = row.querySelector('.audio-play-btn');
        if (playBtn) {
            playBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                toggleAudioPreview(aud, row, playBtn);
            });
        }
        const addBtn = row.querySelector('.audio-add-btn');
        if (addBtn) {
            addBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                addMediaToTimeline(aud, row);
            });
        }
        row.addEventListener('click', () => {
            addMediaToTimeline(aud, row);
        });
        return row;
    }
    function renderAudioBatch(reset = false) {
        const listEl = document.getElementById('media-audio-list');
        const empty = document.getElementById('media-audio-empty');
        if (!listEl) return;
        const list = mediaStore.audio;
        if (!list.length) {
            listEl.innerHTML = '';
            if (empty) empty.style.display = 'block';
            renderedCounts.audio = 0;
            return;
        }
        if (empty) empty.style.display = 'none';
        if (reset || renderedCounts.audio === 0) {
            listEl.innerHTML = '';
            renderedCounts.audio = 0;
        }
        const startIdx = renderedCounts.audio;
        const endIdx = Math.min(list.length, startIdx + PAGE_SIZE);
        if (startIdx >= endIdx) return;
        let idx = startIdx;
        function renderNext() {
            if (activeTab !== 'audio' || idx >= endIdx) return;
            const item = list[idx];
            if (!findElementByPath('.media-audio-item', item.path)) {
                listEl.appendChild(createAudioRow(item));
            }
            idx++;
            renderedCounts.audio = idx;
            if (idx < endIdx) {
                requestAnimationFrame(renderNext);
            } else {
                ensureEnoughContent();
            }
        }
        renderNext();
    }
    function toggleAudioPreview(aud, row, btn) {
        if (previewAudio && playingAudioRow === row) {
            stopAudioPreview();
            return;
        }
        stopAudioPreview();
        const src = aud.blobUrl || aud.path;
        if (!src) return;
        previewAudio = new Audio(src);
        playingAudioRow = row;
        btn.innerHTML = '⏸';
        btn.classList.add('is-playing');
        previewAudio.onended = () => stopAudioPreview();
        previewAudio.onerror = () => stopAudioPreview();
        previewAudio.play().catch(() => stopAudioPreview());
    }
    function stopAudioPreview() {
        if (previewAudio) {
            previewAudio.pause();
            previewAudio.currentTime = 0;
            previewAudio = null;
        }
        if (playingAudioRow) {
            const btn = playingAudioRow.querySelector('.media-audio-play-btn');
            if (btn) {
                btn.innerHTML = '▶';
                btn.classList.remove('is-playing');
            }
            playingAudioRow = null;
        }
    }
    // ========================================================================
    // 7. CHÈN VÀO TIMELINE JSON (LINK TUYỆT ĐỐI)
    // ========================================================================
    function addMediaToTimeline(mediaItem, elItem) {
        const curMs = Math.max(0, Math.round(typeof getAccurateTimeMs === 'function' ? getAccurateTimeMs() : (state.currentTimeMs || 0)));
        const start = curMs;
        let end;
        let newItem;
        const durMs = mediaItem.duration ? Math.round(mediaItem.duration * 1000) : 0;
        const absSrc = mediaItem.path; // Link tuyệt đối (e.g. /Asset/video/Solo.mp4 hoặc https://...)
        if (mediaItem.type === 'video') {
            end = start + (durMs > 0 ? durMs : 5000);
            newItem = {
                type: 'video',
                src: absSrc,
                start: start,
                end: end,
                width: '100%',
                height: '100%',
                objectFit: 'cover',
                volume: 1
            };
        } else if (mediaItem.type === 'image') {
            end = start + 5000;
            newItem = {
                type: 'image',
                src: absSrc,
                start: start,
                end: end,
                width: '100%',
                height: '100%',
                objectFit: 'cover'
            };
        } else if (mediaItem.type === 'audio') {
            end = start + (durMs > 0 ? durMs : 30000);
            newItem = {
                type: 'audio',
                src: absSrc,
                start: start,
                end: end,
                volume: 1
            };
        }
        const rawTimeline = (typeof window.getProjectTabData === 'function')
            ? window.getProjectTabData('timeline')
            : (state && Array.isArray(state.timeline) ? state.timeline : []);
        const timelineList = Array.isArray(rawTimeline) ? [...rawTimeline] : [];
        let insertIdx = timelineList.length;
        for (let i = 0; i < timelineList.length; i++) {
            const it = timelineList[i];
            const s = (it && typeof it.start === 'number') ? it.start : 0;
            if (s > start) {
                insertIdx = i;
                break;
            }
        }
        timelineList.splice(insertIdx, 0, newItem);
        // Đăng ký ngay blob URL với TimelineEngine để live preview hiển thị tức thì
        if (mediaItem.blobUrl) {
            registerAssetWithEngine(mediaItem.path, mediaItem.name, mediaItem.blobUrl);
        }
        const targetSnippet = `"src": "${mediaItem.path}"`;
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
        if (UI.statusText) {
            const typeLabel = mediaItem.type === 'video' ? 'video' : (mediaItem.type === 'audio' ? 'âm thanh' : 'hình ảnh');
            const durSec = Math.round((end - start) / 1000);
            UI.statusText.innerText = `✓ Đã thêm ${typeLabel} "${mediaItem.name}" (+${durSec}s tại ${start}ms)`;
            UI.statusText.classList.add('is-success');
            setTimeout(() => UI.statusText.classList.remove('is-success'), 3000);
        }
    }
    // ========================================================================
    // 8. CHUYỂN TAB & TOGGLE DRAWER
    // ========================================================================
    function updateRailButtons(isOpen) {
        const railMedia = document.getElementById('act-media-btn');
        const railAudio = document.getElementById('act-audio-btn');
        const railText = document.getElementById('act-text-btn');
        const railEffect = document.getElementById('act-effect-btn');
        if (!railMedia && !railAudio && !railText && !railEffect) return;
        if (!isOpen) {
            if (railMedia) railMedia.classList.remove('active');
            if (railAudio) railAudio.classList.remove('active');
            if (railText) railText.classList.remove('active');
            if (railEffect) railEffect.classList.remove('active');
            return;
        }
        if (railMedia) railMedia.classList.toggle('active', activeTab === 'video' || activeTab === 'image');
        if (railAudio) railAudio.classList.toggle('active', activeTab === 'audio');
        if (railText) railText.classList.toggle('active', activeTab === 'font');
        if (railEffect) railEffect.classList.toggle('active', activeTab === 'effect');
    }
    function switchTab(tab, force = false) {
        if (!tab) return;
        if (tab === activeTab && !force && renderedCounts[activeTab] > 0) return;
        const tabBtns = document.querySelectorAll('.media-tab-btn');
        tabBtns.forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
        const targetPaneId = (tab === 'font') ? 'media-tab-fonts' : (tab === 'effect') ? 'media-tab-effects' : `media-tab-${tab}`;
        const panes = document.querySelectorAll('.media-tab-pane');
        panes.forEach(p => p.classList.toggle('active', p.id === targetPaneId));
        activeTab = tab;
        updateRailButtons(true);
        stopAudioPreview();
        stopVideoPreview();
        if (tab === 'video' || tab === 'image' || tab === 'audio') {
            if (renderedCounts[activeTab] === 0 || force) {
                renderCurrentTab(false);
            } else {
                ensureEnoughContent();
            }
        }
    }
    function toggleMediaDrawer() {
        const drawer = document.getElementById('media-drawer');
        const mediaBtn = document.getElementById('media-btn');
        if (!drawer) return;
        const willOpen = !drawer.classList.contains('is-open');
        const aiDrawer = document.getElementById('ai-lyrics-drawer');
        const exportDrawer = document.getElementById('export-drawer');
        const styleDrawer = document.getElementById('style-drawer');
        const aiBtn = document.getElementById('ai-lyrics-btn');
        const exportBtn = document.getElementById('export-btn');
        const styleBtn = document.getElementById('style-btn');
        if (willOpen) {
            if (aiDrawer) aiDrawer.classList.remove('is-open');
            if (exportDrawer) exportDrawer.classList.remove('is-open');
            if (styleDrawer) styleDrawer.classList.remove('is-open');
            if (aiBtn) aiBtn.classList.remove('is-active');
            if (exportBtn) exportBtn.classList.remove('is-active');
            if (styleBtn) {
                styleBtn.classList.remove('is-active');
                styleBtn.setAttribute('aria-expanded', 'false');
            }
        } else {
            stopAudioPreview();
            stopVideoPreview();
        }
        drawer.classList.toggle('is-open', willOpen);
        if (mediaBtn) mediaBtn.classList.toggle('is-active', willOpen);
        updateRailButtons(willOpen);
        if (willOpen) {
            if (renderedCounts[activeTab] === 0) {
                renderCurrentTab(false);
            } else {
                ensureEnoughContent();
            }
        }
        setTimeout(() => {
            if (typeof refreshViewport === 'function') refreshViewport();
        }, 280);
    }
    function openTab(tab) {
        const drawer = document.getElementById('media-drawer');
        if (!drawer) return;
        const isOpen = drawer.classList.contains('is-open');
        if (isOpen && activeTab === tab) {
            toggleMediaDrawer();
        } else {
            if (!isOpen) {
                toggleMediaDrawer();
            }
            if (tab && tab !== activeTab) {
                switchTab(tab);
            } else {
                updateRailButtons(true);
            }
        }
    }
    // ========================================================================
    // 9. EVENT LISTENERS: DOUBLE CLICK, DRAG & DROP, URL INPUT
    // ========================================================================
    function initMediaDrawer() {
        const mediaBtn = document.getElementById('media-btn');
        if (mediaBtn) {
            mediaBtn.addEventListener('click', toggleMediaDrawer);
        }
        // 1. Chuyển Tab (video | image | audio)
        const tabBtns = document.querySelectorAll('.media-tab-btn');
        tabBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                switchTab(btn.dataset.tab);
            });
        });
        // 2. Text box nhập link URL media online
        const urlInput = document.getElementById('media-url-input');
        if (urlInput) {
            urlInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') {
                    e.preventDefault();
                    const val = urlInput.value;
                    handleOnlineUrlInput(val);
                }
            });
        }
        // 3. File input ẩn khi double-click hoặc kéo thả
        const fileInput = document.getElementById('media-file-input');
        if (fileInput) {
            fileInput.addEventListener('change', () => {
                const files = Array.from(fileInput.files || []);
                if (files.length) {
                    importFiles(files, true);
                }
                fileInput.value = '';
            });
        }
        // 4. Double click vào vùng trống trong tab -> mở chọn file
        const drawerBody = document.querySelector('.media-drawer-body');
        if (drawerBody) {
            drawerBody.addEventListener('dblclick', (e) => {
                if (e.target.closest('.media-card') ||
                    e.target.closest('.media-audio-item') ||
                    e.target.closest('button') ||
                    e.target.closest('input')) {
                    return;
                }
                if (fileInput) fileInput.click();
            });
            // 5. Drag & Drop thẳng vào tab kèm lớp phủ mờ có chữ "insert file ABC.."
            let dragCounter = 0;
            const dragOverlay = document.getElementById('media-drag-overlay');
            const dragText = document.getElementById('media-drag-text');
            drawerBody.addEventListener('dragenter', (e) => {
                e.preventDefault();
                dragCounter++;
                if (dragOverlay) {
                    let fileName = '';
                    if (e.dataTransfer && e.dataTransfer.items) {
                        for (const it of e.dataTransfer.items) {
                            if (it.kind === 'file') {
                                const f = it.getAsFile ? it.getAsFile() : null;
                                if (f && f.name) {
                                    fileName = f.name;
                                    break;
                                }
                            }
                        }
                    }
                    if (!fileName && e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length) {
                        fileName = e.dataTransfer.files[0].name;
                    }
                    dragText.textContent = fileName ? `insert file ${fileName}..` : `insert file..`;
                    dragOverlay.classList.add('is-active');
                }
            });
            drawerBody.addEventListener('dragover', (e) => {
                e.preventDefault();
                if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
            });
            drawerBody.addEventListener('dragleave', (e) => {
                e.preventDefault();
                dragCounter--;
                if (dragCounter <= 0) {
                    dragCounter = 0;
                    if (dragOverlay) dragOverlay.classList.remove('is-active');
                }
            });
            drawerBody.addEventListener('drop', async (e) => {
                e.preventDefault();
                dragCounter = 0;
                if (dragOverlay) dragOverlay.classList.remove('is-active');
                const files = Array.from(e.dataTransfer ? e.dataTransfer.files : []);
                if (files.length) {
                    await importFiles(files, true);
                }
            });
            // 6. Cuộn lười biếng (Lazy loading): nạp thêm khi kéo xuống gần đáy
            drawerBody.addEventListener('scroll', onDrawerScroll, { passive: true });
        }
        // Khi load project: chỉ add thêm file mới ngầm, KHÔNG load toàn bộ, KHÔNG cướp statusText
        window.addEventListener('project-loaded', async (e) => {
            const handle = (e.detail && e.detail.dirHandle) || (state && state.dirHandle);
            if (handle) {
                await syncProjectMedia(handle);
            }
        });
        // Tải sẵn dữ liệu toàn cục từ IndexedDB
        initGlobalMediaStore();

        // Lắng nghe sự kiện vi sai từ Trung tâm quản lý tài nguyên (StockCut Resource System)
        if (window.ResourceSystem) {
            window.ResourceSystem.on('add', (desc) => {
                let type = desc.type;
                const ext = (desc.name || desc.path || '').split('.').pop().toLowerCase();
                if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) type = 'audio';
                else if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) type = 'image';
                else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) type = 'video';
                desc.type = type;
                if (mediaStore[type] && !mediaStore[type].some(x => x.path === desc.path || x.id === desc.id)) {
                    mediaStore[type].push(desc);
                    if (type === activeTab && renderedCounts[type] < PAGE_SIZE) {
                        appendSingleItemToCurrentTab(desc, false);
                    }
                }
            });
            window.ResourceSystem.on('remove', (desc) => {
                const type = desc.type;
                if (mediaStore[type]) {
                    const idx = mediaStore[type].findIndex(x => x.path === desc.path || x.id === desc.id);
                    if (idx !== -1) mediaStore[type].splice(idx, 1);
                    const gridId = type === 'video' ? 'media-video-grid' : (type === 'image' ? 'media-image-grid' : 'media-audio-list');
                    const grid = document.getElementById(gridId);
                    if (grid) {
                        const el = grid.querySelector(`[data-id="${CSS.escape(desc.id)}"]`);
                        if (el) el.remove();
                    }
                }
            });
        }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initMediaDrawer);
    } else {
        initMediaDrawer();
    }
    const SFX_ITEMS = [
        { name: "Swish", duration: 1.2, cat: "BCD Studio · Transition", iconColor: "#06b6d4", path: "/Asset/audio/swish.mp3" },
        { name: "Vine Boom", duration: 1.5, cat: "Meme SFX · Impact", iconColor: "#f59e0b", path: "/Asset/audio/vine-boom.mp3" },
        { name: "Magic Reveal", duration: 2.1, cat: "BCD Studio · Magic", iconColor: "#a855f7", path: "/Asset/audio/magic-reveal.mp3" },
        { name: "Crowded People (Indoors)", duration: 104, cat: "Ai-tomo · Ambience", iconColor: "#10b981", path: "/Asset/audio/crowd.mp3" },
        { name: "Whoosh Air Fast", duration: 0.8, cat: "Action · Whoosh", iconColor: "#3b82f6", path: "/Asset/audio/whoosh.mp3" },
        { name: "Camera Shutter", duration: 1.1, cat: "Foley · Click", iconColor: "#ec4899", path: "/Asset/audio/shutter.mp3" },
        { name: "Pop Bubble", duration: 0.5, cat: "Interface · Pop", iconColor: "#14b8a6", path: "/Asset/audio/pop.mp3" },
        { name: "Sub Bass Drop", duration: 2.5, cat: "Impact · Bass", iconColor: "#6366f1", path: "/Asset/audio/bass-drop.mp3" }
    ];
    function renderSfxList() {
        const sfxContainer = document.getElementById('media-sfx-list');
        if (!sfxContainer || sfxContainer.children.length > 0) return;
        SFX_ITEMS.forEach(sfx => {
            const item = {
                type: 'audio',
                name: sfx.name,
                title: sfx.name,
                artist: sfx.cat,
                duration: sfx.duration,
                path: sfx.path
            };
            const row = document.createElement('div');
            row.className = 'audio-track-item';
            row.title = `Click to add hiệu ứng âm thanh "${sfx.name}" vào video`;
            row.innerHTML = `
                <div class="audio-track-cover" style="background: rgba(255, 255, 255, 0.06);">
                    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#828294" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
                        <path d="M9 18V5l12-2v13"></path>
                        <circle cx="6" cy="18" r="3"></circle>
                        <circle cx="18" cy="16" r="3"></circle>
                    </svg>
                </div>
                <div class="audio-track-meta">
                    <span class="audio-track-title">${sfx.name}</span>
                    <span class="audio-track-sub">${formatTime(sfx.duration)} · ${sfx.cat}</span>
                </div>
                <div class="audio-track-actions">
                    <button type="button" class="audio-track-btn" title="Thêm vào video">
                        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"></line><line x1="5" y1="12" x2="19" y2="12"></line></svg>
                    </button>
                </div>
            `;
            row.addEventListener('click', () => {
                addMediaToTimeline(item, row);
            });
            sfxContainer.appendChild(row);
        });
    }
    global.MediaDrawer = {
        importFiles,
        importUrl: handleOnlineUrlInput,
        toggle: toggleMediaDrawer,
        addMedia: addMediaToTimeline,
        openTab,
        switchTab,
        renderVideoBatch,
        renderImageBatch,
        renderSfxList
    };
})(typeof window !== 'undefined' ? window : globalThis);
    // ========================================================================
    