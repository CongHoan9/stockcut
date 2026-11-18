// ============================================================================
// js/reload-shield.js — BỘ LỌC KHÁNG TỰ ĐỘNG RELOAD (UNIVERSAL RELOAD SHIELD)
//
// Chạy độc lập trên MỌI môi trường & nền tảng (VS Code Live Server, Vite,
// Webpack, Browsersync, npm live-server, Docker, Python livereload, v.v.).
//
// Tác dụng:
//   Ngăn chặn triệt để hiện tượng trình duyệt tự ý F5 / reload khi bạn lưu
//   file lyrics.txt, config.json, template.json, timeline.json hoặc thay đổi tài nguyên trong thư mục dự án!
// ============================================================================
(function (global) {
    'use strict';
    // Thời điểm lưu tệp gần nhất (ms)
    global.__lastSaveTimestamp = 0;
    // Cờ bảo vệ toàn cục (mặc định: luôn bảo vệ khi đang mở ứng dụng)
    global.__preventAutoReload = true;
    const Shield = {
        enabled: true,
        // Đánh dấu vừa thực hiện lưu tệp (bảo vệ chống reload trong 6 giây tiếp theo)
        lockSave(durationMs = 6000) {
            global.__lastSaveTimestamp = Date.now() + (durationMs - 6000);
            this._notifyBlockedSave();
        },
        // Bật / tắt bộ lọc
        setPrevent(val) {
            global.__preventAutoReload = !!val;
        },
        _notifyBlockedSave() {
            if (typeof console !== 'undefined' && console.log) {
                console.log('%c[ReloadShield] Đang bảo vệ phiên làm việc: Chặn mọi tín hiệu reload tự động từ Dev Server.', 'color: #10b981; font-weight: bold;');
            }
        },
        // Kiểm tra xem một gói tin WebSocket có phải là lệnh reload cần chặn hay không
        shouldBlock(msgData) {
            if (!this.enabled) return false;
            // Kiểm tra nội dung tin nhắn có chứa chỉ thị reload không
            let isReloadSignal = false;
            if (typeof msgData === 'string') {
                const s = msgData.trim().toLowerCase();
                if (s === 'reload' || s === 'refresh' || s.includes('"type":"full-reload"') || s.includes('"type":"livereload"') || s.includes('filereload')) {
                    isReloadSignal = true;
                }
            } else if (msgData && typeof msgData === 'object') {
                try {
                    const str = JSON.stringify(msgData).toLowerCase();
                    if (str.includes('reload')) isReloadSignal = true;
                } catch (e) { }
            }
            if (!isReloadSignal) return false;
            // 1. Kiểm tra nếu vừa mới lưu file trong vòng 6 giây
            const now = Date.now();
            const timeSinceSave = now - (global.__lastSaveTimestamp || 0);
            if (timeSinceSave < 6000) {
                console.warn(`%c[ReloadShield] Đã chặn tín hiệu reload từ Dev Server (Tệp vừa được lưu cách đây ${(timeSinceSave / 1000).toFixed(1)}s).`, 'color: #f59e0b; font-weight: bold;');
                return true;
            }
            // 2. Kiểm tra nếu người dùng đang mở một thư mục dự án hoặc có dữ liệu chưa lưu trong RAM
            const hasActiveProject = !!(global.state && (global.state.dirHandle || global.state.projectFileHandle || (global.state.ram && (global.state.ram.isLyricsDirty || global.state.ram.isJsonDirty))));
            if (hasActiveProject || global.__preventAutoReload) {
                console.warn('%c[ReloadShield] Đã chặn tín hiệu reload tự động từ Dev Server để bảo vệ không gian làm việc của dự án.', 'color: #3b82f6; font-weight: bold;');
                return true;
            }
            return false;
        }
    };
    global.ReloadShield = Shield;
    // ------------------------------------------------------------------------
    // CAN THIỆP TẦNG MẠNG: Bọc window.WebSocket
    // ------------------------------------------------------------------------
    if (typeof global.WebSocket !== 'undefined') {
        const NativeWebSocket = global.WebSocket;
        const ProxiedWebSocket = function (url, protocols) {
            const ws = (protocols !== undefined)
                ? new NativeWebSocket(url, protocols)
                : new NativeWebSocket(url);
            // Bọc hàm addEventListener('message')
            const originalAddEventListener = ws.addEventListener.bind(ws);
            ws.addEventListener = function (type, listener, options) {
                if (type === 'message' && typeof listener === 'function') {
                    const wrappedListener = function (event) {
                        if (Shield.shouldBlock(event.data)) {
                            // Chặn không cho sự kiện lan truyền tới callback của Dev Server
                            if (event.stopImmediatePropagation) event.stopImmediatePropagation();
                            if (event.stopPropagation) event.stopPropagation();
                            return;
                        }
                        return listener.apply(this, arguments);
                    };
                    return originalAddEventListener(type, wrappedListener, options);
                }
                return originalAddEventListener(type, listener, options);
            };
            // Bọc thuộc tính ws.onmessage (Live Server dùng cú pháp này)
            let userOnMessage = null;
            Object.defineProperty(ws, 'onmessage', {
                get() {
                    return userOnMessage;
                },
                set(fn) {
                    if (typeof fn === 'function') {
                        userOnMessage = function (event) {
                            if (Shield.shouldBlock(event.data)) {
                                return;
                            }
                            return fn.apply(this, arguments);
                        };
                    } else {
                        userOnMessage = fn;
                    }
                },
                configurable: true,
                enumerable: true
            });
            return ws;
        };
        // Kế thừa prototype và constants
        ProxiedWebSocket.prototype = NativeWebSocket.prototype;
        ProxiedWebSocket.CONNECTING = NativeWebSocket.CONNECTING;
        ProxiedWebSocket.OPEN = NativeWebSocket.OPEN;
        ProxiedWebSocket.CLOSING = NativeWebSocket.CLOSING;
        ProxiedWebSocket.CLOSED = NativeWebSocket.CLOSED;
        global.WebSocket = ProxiedWebSocket;
    }
})(typeof window !== 'undefined' ? window : this);
