// ============================================================================
// SESSION DB & RESOURCE CACHE ENGINE (NATRON / OLIVE / PIXIJS v8 ARCHITECTURE)
//   Tham chiếu kiến trúc:
//   - PixiJS v8 (pixijs-dev/src/assets): Resolver + Cache + Loader (promiseCache) + BackgroundLoader
//   - Olive NLE (olive-master/app/node/project): Project UUID/URL + Footage timestamp fingerprint + CancelAtom
//   - Natron (Natron-RB-2.6/Engine/Project.h): Project canonicalizePath + AutoSave/Session snapshot recovery
//
//   Cung cấp:
//   - openDB, saveSessionHandle, getSessionHandle, saveSessionProject, getSessionProject
//   - saveSessionAssets, getSessionAssets, clearSessionAssets
//   - window.ResourceSystem (Resolver, Cache, Loader, BackgroundLoader)
// ============================================================================
const dbName = "LyricAppDB";
const DB_VERSION = 3;
const storeName = "handles";
const snapshotStoreName = "snapshots";
const assetStoreName = "project_assets";
let _dbInstance = null;
let _dbOpenPromise = null;
function openDB() {
    if (_dbInstance) return Promise.resolve(_dbInstance);
    if (_dbOpenPromise) return _dbOpenPromise;
    _dbOpenPromise = new Promise((resolve, reject) => {
        const request = indexedDB.open(dbName, DB_VERSION);
        request.onupgradeneeded = (e) => {
            const db = e.target.result;
            if (!db.objectStoreNames.contains(storeName)) {
                db.createObjectStore(storeName);
            }
            if (!db.objectStoreNames.contains(snapshotStoreName)) {
                db.createObjectStore(snapshotStoreName);
            }
            if (!db.objectStoreNames.contains(assetStoreName)) {
                db.createObjectStore(assetStoreName, { keyPath: 'key' });
            }
        };
        request.onsuccess = () => {
            _dbInstance = request.result;
            _dbInstance.onclose = () => {
                _dbInstance = null;
                _dbOpenPromise = null;
            };
            _dbInstance.onversionchange = () => {
                _dbInstance.close();
                _dbInstance = null;
                _dbOpenPromise = null;
            };
            resolve(_dbInstance);
        };
        request.onerror = () => {
            _dbOpenPromise = null;
            reject(request.error);
        };
    });
    return _dbOpenPromise;
}
async function saveSessionHandle(handle) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(storeName, 'readwrite');
        tx.objectStore(storeName).put(handle, 'dirHandle');
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}
async function getSessionHandle() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(storeName, 'readonly');
        const req = tx.objectStore(storeName).get('dirHandle');
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
    });
}
async function clearSessionHandle() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(storeName, 'readwrite');
        tx.objectStore(storeName).delete('dirHandle');
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}
async function saveSessionProject(snapshot) {
    if (!snapshot || typeof snapshot !== 'object') return;
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(snapshotStoreName, 'readwrite');
        tx.objectStore(snapshotStoreName).put({
            ...snapshot,
            updatedAt: Date.now()
        }, 'lastProject');
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}
async function getSessionProject() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(snapshotStoreName, 'readonly');
        const req = tx.objectStore(snapshotStoreName).get('lastProject');
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
    });
}
// Lưu trữ tập hợp file media (Blob/File) của project đang mở vào IndexedDB
// Giúp reload (F5) khôi phục tức thì 100% hình ảnh/âm thanh/video mà không bị mất liên kết
async function saveSessionAssets(projectName, assetEntries) {
    if (!Array.isArray(assetEntries) || assetEntries.length === 0) return;
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(assetStoreName, 'readwrite');
        const store = tx.objectStore(assetStoreName);
        store.clear();
        for (let i = 0; i < assetEntries.length; i++) {
            const entry = assetEntries[i];
            if (!entry || !entry.key || !entry.blob) continue;
            store.put({
                key: entry.key,
                projectName: projectName || '',
                relPath: entry.relPath || entry.key,
                name: entry.name || entry.key.split('/').pop(),
                blob: entry.blob,
                size: entry.blob.size || 0,
                lastModified: entry.blob.lastModified || 0,
                type: entry.blob.type || ''
            });
        }
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}
async function getSessionAssets(projectName) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(assetStoreName, 'readonly');
        const req = tx.objectStore(assetStoreName).getAll();
        req.onsuccess = () => {
            const all = req.result || [];
            if (!projectName) {
                resolve(all);
                return;
            }
            resolve(all.filter(item => !item.projectName || item.projectName === projectName));
        };
        req.onerror = () => reject(req.error);
    });
}
async function clearSessionAssets() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const tx = db.transaction(assetStoreName, 'readwrite');
        tx.objectStore(assetStoreName).clear();
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}
if (typeof window !== 'undefined') {
    window.openDB = openDB;
    window.saveSessionHandle = saveSessionHandle;
    window.getSessionHandle = getSessionHandle;
    window.clearSessionHandle = clearSessionHandle;
    window.saveSessionProject = saveSessionProject;
    window.getSessionProject = getSessionProject;
    window.saveSessionAssets = saveSessionAssets;
    window.getSessionAssets = getSessionAssets;
    window.clearSessionAssets = clearSessionAssets;
}
