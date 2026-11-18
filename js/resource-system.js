// ============================================================================
// js/resource-system.js — LỚP MÀNG SIÊU MỎNG (ULTRA-THIN JS ADAPTER)
// CHO TRUNG TÂM QUẢN LÝ TÀI NGUYÊN C++ (STOCKCUT RESOURCE REGISTRY)
//
// Tuân thủ triệt để Quy tắc 9 (GEMINI.md):
//   • C++ LÀ BỘ NÃO DUY NHẤT: Toàn bộ bảng từ điển tài nguyên (ResourceEntry),
//     khóa băm Zobrist 64-bit, cấu trúc MultiArray 64-byte căn lề, quản lý tuổi thọ
//     thế hệ (LRU Generation Aging), hạn mức RAM Budget 256MB và lập lịch byte-range
//     nằm 100% trong C++ Core (native/core/resource_registry.cpp & stream_pipeline.cpp).
//   • JS KHÔNG NẮM NGHIỆP VỤ: JS chỉ là lớp màng mỏng chuyển tiếp lời gọi từ trình duyệt,
//     lắng nghe sự kiện (ADD, REMOVE, UPDATE, EVICT) từ C++ và cập nhật DOM tương ứng.
//   • ZERO-BULK-PRELOAD: Tuyệt đối không nạp nguyên file media vào RAM. Mọi tệp trên ổ
//     cứng (C:/..., D:/..., /Asset/...) được ánh xạ stream trực tiếp qua Range Requests (206).
// ============================================================================

(function (global) {
    'use strict';

    // ------------------------------------------------------------------------
    // 1. RESOLVER ĐƯỜNG DẪN STREAMING TRỰC TIẾP TỪ Ổ ĐĨA (OLIVE / RANGE HTTP)
    // ------------------------------------------------------------------------

    function normalizePath(rawPath) {
        if (!rawPath || typeof rawPath !== 'string') return '';
        let s = rawPath.trim().replace(/\\/g, '/');
        if (s.startsWith('data:') || s.startsWith('blob:') || s.startsWith('http://') || s.startsWith('https://')) {
            return s;
        }
        if (s.startsWith('file:///')) {
            s = s.slice(8);
        }
        if (/^\/[a-zA-Z]:/.test(s)) {
            s = s.slice(1);
        }
        return s;
    }

    // Bộ đệm URL người dùng cấp quyền truy cập thư mục (File System Access API & Blob mapping)
    const _userHandleUrlMap = new Map();
    let _workspaceRootPath = 'C:/Users/84352/Desktop/Video';

    function registerUserFileUrl(absolutePath, objectUrl) {
        if (!absolutePath || !objectUrl) return;
        const norm = normalizePath(absolutePath);
        _userHandleUrlMap.set(norm, objectUrl);
        _userHandleUrlMap.set(norm.toLowerCase(), objectUrl);
        const fileName = norm.split('/').filter(Boolean).pop();
        if (fileName) {
            _userHandleUrlMap.set(fileName, objectUrl);
            _userHandleUrlMap.set(fileName.toLowerCase(), objectUrl);
        }
    }

    function setWorkspaceRoot(rootPath) {
        if (!rootPath || typeof rootPath !== 'string') return;
        _workspaceRootPath = normalizePath(rootPath).replace(/\/+$/, '');
    }

    // Tự động phát hiện thư mục gốc workspace từ danh sách manifest
    function detectWorkspaceRootFromManifest(manifestData, manifestUrl) {
        if (!manifestData || typeof manifestData !== 'object') return;
        let samplePath = '';
        const types = ['video', 'image', 'audio', 'font'];
        for (const t of types) {
            const list = manifestData[t];
            if (Array.isArray(list) && list.length > 0) {
                const item = list[0];
                const p = (typeof item === 'string') ? item : (item.path || item.src);
                if (p && /^[a-zA-Z]:/.test(p)) {
                    samplePath = p.replace(/\\/g, '/');
                    break;
                }
            }
        }
        if (samplePath) {
            const mNorm = (manifestUrl || 'Asset/manifest.json').replace(/\\/g, '/').replace(/^\/+/, '');
            const mFolder = mNorm.includes('/') ? mNorm.slice(0, mNorm.lastIndexOf('/')) : '';
            if (mFolder && samplePath.includes('/' + mFolder + '/')) {
                const idx = samplePath.indexOf('/' + mFolder + '/');
                _workspaceRootPath = samplePath.slice(0, idx);
            }
        }
    }

    function resolveFileUrl(src) {
        if (!src || typeof src !== 'string') return '';
        const s = src.trim();
        if (s.startsWith('http://') || s.startsWith('https://') || s.startsWith('data:') || s.startsWith('blob:')) {
            return s;
        }
        let path = s.replace(/\\/g, '/');
        if (path.startsWith('file:///')) {
            path = path.slice(8);
        }
        if (/^\/[a-zA-Z]:/.test(path)) {
            path = path.slice(1);
        }
        // 1. Kiểm tra xem đường dẫn này đã có Blob URL từ File System Access API hay chưa (cho bất kỳ thư mục nào ngoài ổ đĩa)
        if (_userHandleUrlMap.has(path)) {
            return _userHandleUrlMap.get(path);
        }
        if (_userHandleUrlMap.has(path.toLowerCase())) {
            return _userHandleUrlMap.get(path.toLowerCase());
        }
        const fileName = path.split('/').filter(Boolean).pop();
        if (fileName && _userHandleUrlMap.has(fileName.toLowerCase())) {
            return _userHandleUrlMap.get(fileName.toLowerCase());
        }
        // 2. Nếu file nằm trong workspace hiện tại (so khớp không phân biệt hoa thường theo chuẩn Windows,
        // hoạt động với bất kỳ thư mục nào người dùng đặt trong project), chuyển đổi thành URL tương đối
        // so với server root để Live Server 5500, Python 8080, Node, Vite đều stream mượt mà
        if (_workspaceRootPath && path.toLowerCase().startsWith(_workspaceRootPath.toLowerCase())) {
            const rel = path.slice(_workspaceRootPath.length);
            return rel.startsWith('/') ? rel : '/' + rel;
        }
        // 3. Tự động nhận diện thư mục Asset hoặc src trong project bất kể workspace đặt ở đâu
        const assetIdx = path.toLowerCase().indexOf('/asset/');
        if (assetIdx !== -1) {
            return path.slice(assetIdx);
        }
        if (path.toLowerCase().startsWith('asset/')) {
            return '/' + path;
        }
        const srcIdx = path.toLowerCase().indexOf('/src/');
        if (srcIdx !== -1) {
            return path.slice(srcIdx);
        }
        if (path.toLowerCase().startsWith('src/')) {
            return '/' + path;
        }
        if (path.startsWith('/@fs/')) {
            return path;
        }
        // 4. Đường dẫn tuyệt đối ngoài workspace: fallback streaming qua endpoint server /@fs/
        const driveMatch = path.match(/^([a-zA-Z]:)(.*)$/);
        if (driveMatch) {
            const drive = driveMatch[1];
            const rest = driveMatch[2];
            return `/@fs/${drive}${rest}`;
        }
        return path;
    }

    // Trình hỗ trợ mở & cấp quyền thư mục người dùng bất kỳ (File System Access API)
    async function openDirectoryPicker() {
        if (!window.showDirectoryPicker) {
            console.warn('[ResourceSystem] File System Access API không được hỗ trợ trên trình duyệt này');
            return null;
        }
        try {
            const dirHandle = await window.showDirectoryPicker({ mode: 'read' });
            async function scanDir(handle, currentPath) {
                for await (const entry of handle.values()) {
                    if (entry.kind === 'file') {
                        const file = await entry.getFile();
                        const blobUrl = URL.createObjectURL(file);
                        const fullPath = (currentPath + '/' + entry.name).replace(/\\/g, '/');
                        registerUserFileUrl(fullPath, blobUrl);
                        registerUserFileUrl(entry.name, blobUrl);
                    } else if (entry.kind === 'directory') {
                        await scanDir(entry, currentPath + '/' + entry.name);
                    }
                }
            }
            await scanDir(dirHandle, dirHandle.name);
            console.log(`[ResourceSystem] Đã nạp thành công thư mục người dùng: ${dirHandle.name}`);
            return dirHandle;
        } catch (e) {
            if (e.name !== 'AbortError') {
                console.warn('[ResourceSystem] Lỗi khi mở thư mục người dùng:', e);
            }
            return null;
        }
    }

    // ------------------------------------------------------------------------
    // 1B. CẦU NỐI Ổ CỨNG CỤC BỘ ~/.stockcut (ZERO-RAM DIRECT DISK ACCESS)
    //     - Phân tách rạch ròi:
    //       + Built-in Assets (/Asset/...): Cấp sẵn trên GitHub / Render / CDN
    //       + Local User Disk (C:/..., D:/...): Lưu link tuyệt đối trong ~/.stockcut/
    // ------------------------------------------------------------------------
    let _bootstrapPromise = null;
    let _bootstrapCache = null;

    const StockCutDiskBridge = {
        get cachedBootstrap() {
            return _bootstrapCache;
        },
        async bootstrap(force = false) {
            if (_bootstrapCache && !force) return _bootstrapCache;
            if (_bootstrapPromise && !force) return _bootstrapPromise;
            _bootstrapPromise = (async () => {
                try {
                    const resp = await fetch('/@stockcut/bootstrap', { cache: 'no-store' });
                    if (resp && resp.ok) {
                        const data = await resp.json();
                        if (data && data.anchor && data.anchor.workspaceRoot) {
                            setWorkspaceRoot(data.anchor.workspaceRoot);
                        }
                        _bootstrapCache = data;
                        return data;
                    }
                } catch (_) {}
                return null;
            })();
            return _bootstrapPromise;
        },
        async loadProject(pathOrName) {
            if (!pathOrName) return null;
            try {
                const resp = await fetch(`/@stockcut/project?path=${encodeURIComponent(pathOrName)}`, { cache: 'no-store' });
                if (resp && resp.ok) {
                    const res = await resp.json();
                    return res && res.ok ? res.project : null;
                }
            } catch (_) {}
            return null;
        },
        async saveProject(payload) {
            try {
                const resp = await fetch('/@stockcut/project/save', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload || {})
                });
                if (resp && resp.ok) return await resp.json();
            } catch (_) {}
            return { ok: false };
        },
        async updateWorkspace(updates) {
            try {
                const resp = await fetch('/@stockcut/workspace', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(updates || {})
                });
                if (resp && resp.ok) return await resp.json();
            } catch (_) {}
            return { ok: false };
        },
        async addLinks(items) {
            try {
                const list = Array.isArray(items) ? items : [items];
                const resp = await fetch('/@stockcut/links/add', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ items: list })
                });
                if (resp && resp.ok) return await resp.json();
            } catch (_) {}
            return { ok: false, items: [] };
        },
        async removeLink(pathOrId) {
            try {
                const resp = await fetch('/@stockcut/links/remove', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ path: pathOrId, id: pathOrId })
                });
                if (resp && resp.ok) return await resp.json();
            } catch (_) {}
            return { ok: false };
        },
        async resolveLocalFile(name, size) {
            try {
                const resp = await fetch('/@stockcut/links/resolve', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ name, size: size || 0 })
                });
                if (resp && resp.ok) {
                    const data = await resp.json();
                    return data && data.ok ? data.path : '';
                }
            } catch (_) {}
            return '';
        }
    };

    // Khởi chạy bootstrap ngay lập tức song song với quá trình tải trang
    if (typeof window !== 'undefined' && window.location && window.location.protocol !== 'file:') {
        StockCutDiskBridge.bootstrap();
    }

    global.resolveFileUrl = resolveFileUrl;
    global.registerUserFileUrl = registerUserFileUrl;
    global.setWorkspaceRoot = setWorkspaceRoot;
    global.openDirectoryPicker = openDirectoryPicker;
    global.StockCutDiskBridge = StockCutDiskBridge;

    const MEDIA_TYPE_MAP = {
        video: 0,
        image: 1,
        audio: 2,
        font: 3
    };

    const TYPE_NAME_MAP = ['video', 'image', 'audio', 'font'];

    // ------------------------------------------------------------------------
    // 2. ULTRA-THIN RESOURCE ADAPTER (DELEGATING TO C++ RESOURCE REGISTRY)
    // ------------------------------------------------------------------------

    class StockCutResourceAdapter {
        constructor() {
            // Bộ đệm giải mã hình ảnh & âm thanh nhẹ (Chỉ nạp khi render frame hoặc playback)
            this._imageCache = new Map();         // url -> { img, bytes }
            this._audioBufferCache = new Map();    // url -> { audioBuffer, bytes }
            this._arrayBufferCache = new Map();    // url -> { buffer, bytes }
            this._bassBakeCache = new Map();       // key -> { envelope }

            // Thùng dữ liệu dự phòng trước khi WASM tải xong
            this._earlyQueue = [];
            this._earlyCatalog = {
                type: ['All', 'Phonk', 'Background music', 'Pop', 'Chill', 'R&B', 'High Tempo'],
                Categories: ['RECOMMEND', 'POP', 'R&B', 'HEALING', 'WARM', 'HIGH TEMPO'],
                Recommended: []
            };

            this._listeners = {
                add: new Set(),
                remove: new Set(),
                change: new Set()
            };

            this._isManifestLoaded = false;
            this._projectEpoch = 1;

            // Tự động kết nối với C++ WasmBridge khi khởi tạo xong
            this._initNativeBinding();
        }

        async _initNativeBinding() {
            if (global.WasmBridge && typeof global.WasmBridge.whenReady === 'function') {
                try {
                    await global.WasmBridge.whenReady();
                    // Flush toàn bộ các mục nạp sớm vào C++ Core
                    if (this._earlyQueue.length > 0 && global.WasmBridge.resources) {
                        for (const item of this._earlyQueue) {
                            this._registerToCpp(item.item, item.extra);
                        }
                        this._earlyQueue = [];
                    }
                    // Bơm sự kiện từ C++ sang JS
                    this.pollEvents();
                } catch (e) {
                    console.warn('[ResourceAdapter] Native binding warning:', e);
                }
            }
        }

        // --- Event Subscription ---
        on(event, fn) {
            if (this._listeners[event]) this._listeners[event].add(fn);
        }

        off(event, fn) {
            if (this._listeners[event]) this._listeners[event].delete(fn);
        }

        _emit(event, data) {
            if (this._listeners[event]) {
                for (const fn of this._listeners[event]) {
                    try { fn(data); } catch (e) { console.warn(`[ResourceAdapter] Event '${event}' handler error:`, e); }
                }
            }
        }

        // --- C++ Event Poller ---
        pollEvents() {
            if (!global.WasmBridge || !global.WasmBridge.resources || typeof global.WasmBridge.resources.pollEvent !== 'function') {
                return;
            }
            let ev;
            while ((ev = global.WasmBridge.resources.pollEvent())) {
                const entry = global.WasmBridge.resources.getEntry(ev.resourceId);
                if (!entry) continue;
                this._decorateEntry(entry);
                if (ev.eventType === 1) { // ADD
                    this._emit('add', entry);
                } else if (ev.eventType === 2) { // REMOVE
                    this._emit('remove', entry);
                } else if (ev.eventType === 3) { // UPDATE
                    this._emit('change', entry);
                } else if (ev.eventType === 4) { // EVICT (C++ ra lệnh thu hồi RAM)
                    this._evictDecodedMemory(entry.url, entry.key);
                }
            }
        }

        _decorateEntry(entry) {
            entry.url = resolveFileUrl(entry.path);
            entry.blobUrl = entry.url;
            entry.thumb = entry.type === 'image' ? entry.url : null;
            entry.audioGenre = entry.genre || 'All';
            return entry;
        }

        _registerToCpp(item, extra = {}) {
            if (!global.WasmBridge || !global.WasmBridge.resources) return null;
            const rawPath = item.path || item.src || (extra && extra.path) || '';
            const canonicalPath = normalizePath(rawPath);
            if (!canonicalPath) return null;

            const fileName = item.name || canonicalPath.split('/').filter(Boolean).pop() || 'resource';
            const ext = fileName.split('.').pop().toLowerCase();

            let mediaType = 0;
            if (item.type) {
                mediaType = MEDIA_TYPE_MAP[item.type] ?? 0;
            } else {
                if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) mediaType = 1;
                else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) mediaType = 2;
                else if (['ttf', 'otf', 'woff', 'woff2'].includes(ext)) mediaType = 3;
            }

            let flags = 0;
            if (item.isAsset || canonicalPath.includes('Asset/')) flags |= 1; // RES_FLAG_IS_ASSET
            if (item.isProjectFile) flags |= 2; // RES_FLAG_IS_PROJECT
            flags |= 8; // RES_FLAG_STREAMABLE

            const size = item.size || extra.size || (extra.file && extra.file.size) || 0;
            const durationMs = item.durationMs || (item.duration ? Math.round(item.duration * 1000) : 0);
            const title = item.title || item.name || fileName;
            const artist = item.artist || item.cat || (mediaType === 2 ? 'Audio' : '');
            const genre = item.genre || item.audioGenre || item.type || 'All';

            const resId = global.WasmBridge.resources.register(
                mediaType,
                canonicalPath,
                fileName,
                title,
                artist,
                genre,
                size,
                durationMs,
                item.width || 0,
                item.height || 0,
                flags
            );

            if (resId) {
                const entry = global.WasmBridge.resources.getEntry(resId);
                if (entry) {
                    this._decorateEntry(entry);
                    if (extra.blob || extra.file) entry.blob = extra.blob || extra.file;
                    if (extra.handle) entry.handle = extra.handle;
                    return entry;
                }
            }
            return null;
        }

        // --- Core Resource Lifecycle (Zero Business in JS, 100% C++) ---
        register(item, extra = {}) {
            if (!item) return null;
            if (!item.type) {
                const ext = (item.name || item.path || item.src || '').split('.').pop().toLowerCase();
                if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) item.type = 'image';
                else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) item.type = 'audio';
                else if (['ttf', 'otf', 'woff', 'woff2'].includes(ext)) item.type = 'font';
                else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) item.type = 'video';
            }
            if (global.WasmBridge && global.WasmBridge.isReady() && global.WasmBridge.resources) {
                const entry = this._registerToCpp(item, extra);
                this.pollEvents();
                return entry;
            }

            // Lưu tạm vào earlyQueue nếu WASM chưa sẵn sàng
            this._earlyQueue.push({ item, extra });
            const rawPath = item.path || item.src || '';
            const canonicalPath = normalizePath(rawPath);
            const fileName = item.name || canonicalPath.split('/').filter(Boolean).pop() || 'resource';
            const streamUrl = resolveFileUrl(canonicalPath);
            const entry = {
                id: `res_${item.type || 'video'}_${fileName.replace(/[^a-zA-Z0-9]/g, '_')}`,
                key: canonicalPath,
                path: rawPath,
                name: fileName,
                title: item.title || fileName,
                artist: item.artist || '',
                genre: item.genre || item.type || 'All',
                type: item.type || 'video',
                size: item.size || 0,
                duration: item.duration || 0,
                url: streamUrl,
                blobUrl: streamUrl,
                thumb: item.type === 'image' ? streamUrl : null,
                isDisk: true,
                isAsset: !!(item.isAsset || canonicalPath.includes('Asset/')),
                isProjectFile: !!item.isProjectFile
            };
            this._emit('add', entry);
            return entry;
        }

        unregister(keyOrId) {
            if (!keyOrId) return false;
            if (global.WasmBridge && global.WasmBridge.isReady() && global.WasmBridge.resources) {
                let resId = 0;
                if (typeof keyOrId === 'number') {
                    resId = keyOrId;
                } else {
                    resId = global.WasmBridge.resources.findByPath(normalizePath(keyOrId));
                }
                if (resId) {
                    const ok = global.WasmBridge.resources.unregister(resId);
                    this.pollEvents();
                    return ok;
                }
            }
            return false;
        }

        resolveDescriptor(src) {
            if (!src) return null;
            const norm = normalizePath(src);
            if (global.WasmBridge && global.WasmBridge.isReady() && global.WasmBridge.resources) {
                let resId = global.WasmBridge.resources.findByPath(norm);
                if (!resId && norm !== src) {
                    resId = global.WasmBridge.resources.findByPath(src);
                }
                if (!resId && global.WasmBridge.resources.findByName) {
                    resId = global.WasmBridge.resources.findByName(src);
                }
                if (resId) {
                    const entry = global.WasmBridge.resources.getEntry(resId);
                    return entry ? this._decorateEntry(entry) : null;
                }
            }
            for (const q of this._earlyQueue) {
                if (normalizePath(q.item.path) === norm || q.item.name === src) {
                    return {
                        key: norm,
                        path: q.item.path,
                        url: resolveFileUrl(q.item.path),
                        name: q.item.name,
                        type: q.item.type
                    };
                }
            }
            return null;
        }

        resolveUrl(src) {
            if (!src) return '';
            const desc = this.resolveDescriptor(src);
            if (desc && desc.url) return desc.url;
            return resolveFileUrl(src);
        }

        list(type) {
            const mType = MEDIA_TYPE_MAP[type] ?? 0;
            if (global.WasmBridge && global.WasmBridge.isReady() && global.WasmBridge.resources) {
                const count = global.WasmBridge.resources.getCountByType(mType);
                const out = [];
                for (let i = 0; i < count; i++) {
                    const resId = global.WasmBridge.resources.getIdByTypeIndex(mType, i);
                    if (resId) {
                        const entry = global.WasmBridge.resources.getEntry(resId);
                        if (entry) out.push(this._decorateEntry(entry));
                    }
                }
                return out;
            }
            // Fallback danh sách nạp sớm (Phân loại chính xác theo đuôi file)
            return this._earlyQueue
                .filter(q => {
                    let t = q.item.type;
                    if (!t) {
                        const ext = (q.item.name || q.item.path || q.item.src || '').split('.').pop().toLowerCase();
                        if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) t = 'image';
                        else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) t = 'audio';
                        else if (['ttf', 'otf', 'woff', 'woff2'].includes(ext)) t = 'font';
                        else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) t = 'video';
                        else t = 'video';
                    }
                    return t === type;
                })
                .map(q => {
                    const p = normalizePath(q.item.path);
                    return {
                        id: `early_${type}_${q.item.name}`,
                        key: p,
                        path: q.item.path,
                        name: q.item.name,
                        title: q.item.title || q.item.name,
                        artist: q.item.artist || '',
                        type,
                        size: q.item.size || 0,
                        duration: q.item.duration || 0,
                        url: resolveFileUrl(p),
                        blobUrl: resolveFileUrl(p),
                        thumb: type === 'image' ? resolveFileUrl(p) : null
                    };
                });
        }

        getAudioCatalog() {
            const recommended = this.list('audio');
            return {
                type: [...this._earlyCatalog.type],
                Categories: [...this._earlyCatalog.Categories],
                Recommended: recommended.length > 0 ? recommended : [...this._earlyCatalog.Recommended]
            };
        }

        // --- Manifest Reader (Nạp 1 lần vào C++ Core Engine từ ~/.stockcut + Asset/manifest.json) ---
        async loadManifest(manifestUrl = 'Asset/manifest.json') {
            if (this._isManifestLoaded) {
                return this.getAudioCatalog();
            }
            try {
                let data = null;
                const boot = await StockCutDiskBridge.bootstrap();
                if (boot && boot.builtinManifest) {
                    data = boot.builtinManifest;
                } else {
                    const res = await fetch(new URL(manifestUrl, document.baseURI).href);
                    if (!res.ok) throw new Error(`HTTP ${res.status}`);
                    data = await res.json();
                }
                if (!data) return null;
                detectWorkspaceRootFromManifest(data, manifestUrl);

                // 1. Đăng ký tài nguyên cấp sẵn (/Asset/... - Built-in Cloud Assets cho GitHub/Render)
                if (Array.isArray(data.video)) {
                    for (const v of data.video) {
                        this.register({ ...v, type: 'video', isAsset: true, scope: 'builtin' });
                    }
                }
                if (Array.isArray(data.image)) {
                    for (const im of data.image) {
                        this.register({ ...im, type: 'image', isAsset: true, scope: 'builtin' });
                    }
                }
                if (data.audio) {
                    let audioConf = Array.isArray(data.audio) ? data.audio[0] : data.audio;
                    if (audioConf) {
                        if (Array.isArray(audioConf.type)) this._earlyCatalog.type = audioConf.type;
                        if (Array.isArray(audioConf.Categories)) this._earlyCatalog.Categories = audioConf.Categories;
                        if (Array.isArray(audioConf.Recommended)) {
                            for (const aud of audioConf.Recommended) {
                                this.register({ ...aud, type: 'audio', isAsset: true, scope: 'builtin' });
                            }
                        }
                    }
                }

                // 2. Đăng ký các link tuyệt đối của người dùng từ ~/.stockcut/ (media_links.json & audio_links.json)
                if (boot) {
                    if (Array.isArray(boot.media)) {
                        for (const m of boot.media) {
                            this.register({ ...m, isAsset: false, scope: 'local' });
                        }
                    }
                    if (Array.isArray(boot.audio)) {
                        for (const a of boot.audio) {
                            this.register({ ...a, type: 'audio', isAsset: false, scope: 'local' });
                        }
                    }
                }

                this._isManifestLoaded = true;
                this.pollEvents();
                global.AudioCatalog = this.getAudioCatalog();
                return data;
            } catch (e) {
                console.warn('[ResourceAdapter] loadManifest warning:', e);
                return null;
            }
        }

        // --- Memory Decoded Cache Eviction (Enforced by C++ Aging) ---
        _evictDecodedMemory(url, key) {
            if (url && this._imageCache.has(url)) {
                this._imageCache.delete(url);
            }
            if (url && this._audioBufferCache.has(url)) {
                this._audioBufferCache.delete(url);
            }
            if (url && this._arrayBufferCache.has(url)) {
                this._arrayBufferCache.delete(url);
            }
        }

        // --- On-Demand Decoded Media Access (Only when requested) ---
        async loadArrayBuffer(src) {
            if (!src) throw new Error('Empty asset source');
            const url = this.resolveUrl(src);
            if (this._arrayBufferCache.has(url)) {
                return this._arrayBufferCache.get(url).slice(0);
            }
            const res = await fetch(url);
            if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
            const ab = await res.arrayBuffer();
            if (ab && ab.byteLength > 0 && ab.byteLength <= 32 * 1024 * 1024) {
                this._arrayBufferCache.set(url, ab);
                const desc = this.resolveDescriptor(src);
                if (desc && desc.resourceId && global.WasmBridge && global.WasmBridge.resources) {
                    global.WasmBridge.resources.setMemoryLoaded(desc.resourceId, ab.byteLength);
                }
            }
            return ab.slice(0);
        }

        async loadDecodedAudio(src, decodeCtx) {
            if (!src) throw new Error('Empty audio source');
            const url = this.resolveUrl(src);
            if (this._audioBufferCache.has(url)) {
                return this._audioBufferCache.get(url);
            }
            const ab = await this.loadArrayBuffer(src);
            const OAC = global.OfflineAudioContext || global.webkitOfflineAudioContext;
            const AC = global.AudioContext || global.webkitAudioContext;
            const ctx = decodeCtx || (OAC ? new OAC(2, 44100, 44100) : (AC ? new AC() : null));
            if (!ctx) throw new Error('WebAudio Context unavailable');
            const audioBuf = await ctx.decodeAudioData(ab);
            this._audioBufferCache.set(url, audioBuf);
            const bytes = audioBuf.length * audioBuf.numberOfChannels * 4;
            const desc = this.resolveDescriptor(src);
            if (desc && desc.resourceId && global.WasmBridge && global.WasmBridge.resources) {
                global.WasmBridge.resources.setMemoryLoaded(desc.resourceId, bytes);
            }
            return audioBuf;
        }

        getCachedImage(src) {
            if (!src) return null;
            const url = this.resolveUrl(src);
            const img = this._imageCache.get(url) || this._imageCache.get(src);
            if (img && img.complete && img.naturalWidth > 0) {
                const desc = this.resolveDescriptor(src);
                if (desc && desc.resourceId && global.WasmBridge && global.WasmBridge.resources) {
                    global.WasmBridge.resources.touch(desc.resourceId);
                }
                return img;
            }
            return null;
        }

        async loadDecodedImage(src, onLoaded) {
            if (!src) return null;
            const url = this.resolveUrl(src);
            const existing = this._imageCache.get(url);
            if (existing && existing.complete && existing.naturalWidth > 0) {
                if (typeof onLoaded === 'function') onLoaded(existing);
                return existing;
            }

            return new Promise((resolve) => {
                const im = new Image();
                im.crossOrigin = 'anonymous';
                const finish = (ok) => {
                    if (ok && im.naturalWidth > 0) {
                        this._imageCache.set(url, im);
                        this._imageCache.set(src, im);
                        const bytes = im.naturalWidth * im.naturalHeight * 4;
                        const desc = this.resolveDescriptor(src);
                        if (desc && desc.resourceId && global.WasmBridge && global.WasmBridge.resources) {
                            global.WasmBridge.resources.setMemoryLoaded(desc.resourceId, bytes);
                        }
                        if (typeof onLoaded === 'function') onLoaded(im);
                        resolve(im);
                    } else {
                        resolve(null);
                    }
                };
                im.onload = () => {
                    if (typeof im.decode === 'function') {
                        im.decode().then(() => finish(true)).catch(() => finish(true));
                    } else {
                        finish(true);
                    }
                };
                im.onerror = () => finish(false);
                im.src = url;
            });
        }

        // --- Project Session Management ---
        nextProjectEpoch() {
            return ++this._projectEpoch;
        }

        currentProjectEpoch() {
            return this._projectEpoch;
        }

        clearProjectAliases(revokeBlobs = false) {
            if (global.WasmBridge && global.WasmBridge.isReady() && global.WasmBridge.resources) {
                global.WasmBridge.resources.clearProjectFiles();
                this.pollEvents();
            }
        }

        exportSessionAssetEntries() {
            return [];
        }

        get bassBakeCache() {
            return this._bassBakeCache;
        }

        registerAsset(pathOrKey, urlOrBlob, extra = {}) {
            let blob = extra.blob || extra.file || null;
            let url = typeof urlOrBlob === 'string' ? urlOrBlob : '';
            if (urlOrBlob instanceof Blob) {
                blob = urlOrBlob;
            }
            const fileName = extra.name || (typeof pathOrKey === 'string' ? pathOrKey.split('/').pop() : '');
            const ext = fileName.split('.').pop().toLowerCase();
            let inferredType = extra.type;
            if (!inferredType) {
                if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'svg', 'bmp'].includes(ext)) inferredType = 'image';
                else if (['mp3', 'wav', 'm4a', 'ogg', 'flac', 'aac', 'wma'].includes(ext)) inferredType = 'audio';
                else if (['ttf', 'otf', 'woff', 'woff2'].includes(ext)) inferredType = 'font';
                else if (['mp4', 'webm', 'mov', 'mkv', 'avi', 'm4v'].includes(ext)) inferredType = 'video';
                else inferredType = 'video';
            }
            return this.register({
                path: pathOrKey,
                src: pathOrKey,
                name: fileName,
                type: inferredType,
                size: (blob && blob.size) || extra.byteLength || 0,
                duration: extra.duration || 0,
                isProjectFile: !!extra.isProjectFile
            }, {
                blob,
                url,
                file: extra.file,
                handle: extra.handle
            });
        }
    }

    const instance = new StockCutResourceAdapter();
    global.ResourceSystem = instance;
    global.StockCutResourceAdapter = StockCutResourceAdapter;

})(typeof window !== 'undefined' ? window : this);
