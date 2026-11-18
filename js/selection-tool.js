// ============================================================================
// SELECTION TOOL (CHẾ ĐỘ CON TRỎ CHUỘT / POINTER SELECTION - VIEW ONLY / DEBUG)
//   - C++ Independent Internal Engine Core (GEMINI.md Rule 9: JS Zero-Business)
//   - Chức năng thuần túy xem bố cục ranh giới (bounding box/outlines) hỗ trợ debug.
//   - Chặn 100% di chuyển và xóa: Không can thiệp sửa đổi timeline, không do_edit.
//   - 100% Direct C++ WebGL Overlay: Toàn bộ đường kẻ viền được vẽ trực tiếp đè lên
//     khung hình hiện tại qua WebGL Shader trong C++ Render.
//   - Triệt tiêu 100% xáo trộn DOM: Không tạo/sửa đổi thẻ DOM hay SVG nào trên stage.
//   - Zero Export Contamination: Khâu xuất video (Export) tuyệt đối không sót line nào.
//   - Tự động xóa mọi line (unfocus) ngay khi thao tác ra ngoài hoặc click vùng trống.
// ============================================================================

(function (window) {
    'use strict';

    const SelectionTool = {
        selectedIndices: new Set(), // Set của top-level item index trong fullProjectData.timeline
        isMarquee: false,
        marqueeStart: { x: 0, y: 0 },
        marqueeCurrent: { x: 0, y: 0 },
        marqueeClientStart: { x: 0, y: 0 },

        init() {
            this.bindEvents();
        },

        getStageDimensions() {
            const rc = (window.state && window.state.renderConfig) || { baseW: 1920, baseH: 1080 };
            const bw = rc.baseW || 1920;
            const bh = rc.baseH || 1080;
            return { w: bw, h: bh };
        },

        getDisplayScale() {
            const vp = UI.viewport || document.getElementById('render-viewport');
            if (!vp) return 1.0;
            const rect = vp.getBoundingClientRect();
            const { w } = this.getStageDimensions();
            return (rect && rect.width > 0 && w > 0) ? (rect.width / w) : 1.0;
        },

        updateMarqueeBox(clientX0, clientY0, clientX1, clientY1) {
            const box = document.getElementById('selection-marquee-box');
            if (!box) return;
            const stage = UI.fullscreenStage || document.getElementById('fullscreen-stage') || document.body;
            const stageRect = stage.getBoundingClientRect();
            const left = Math.min(clientX0, clientX1) - stageRect.left;
            const top = Math.min(clientY0, clientY1) - stageRect.top;
            const width = Math.abs(clientX1 - clientX0);
            const height = Math.abs(clientY1 - clientY0);
            box.style.left = left + 'px';
            box.style.top = top + 'px';
            box.style.width = width + 'px';
            box.style.height = height + 'px';
            box.style.display = 'block';
        },

        hideMarqueeBox() {
            const box = document.getElementById('selection-marquee-box');
            if (box) {
                box.style.display = 'none';
                box.style.width = '0px';
                box.style.height = '0px';
            }
        },

        clientToStage(clientX, clientY) {
            const vp = UI.viewport || document.getElementById('render-viewport');
            if (!vp) return { x: 0, y: 0 };
            const rect = vp.getBoundingClientRect();
            const { w, h } = this.getStageDimensions();
            const x = ((clientX - rect.left) / rect.width) * w;
            const y = ((clientY - rect.top) / rect.height) * h;
            return { x, y };
        },

        hitTest(stageX, stageY) {
            if (window.WasmBridge && window.WasmBridge.selection) {
                const hit = window.WasmBridge.selection.hitTest(stageX, stageY);
                if (hit >= 0) return hit;
            }
            return -1;
        },

        marqueeHitTest(x0, y0, x1, y1) {
            if (window.WasmBridge && window.WasmBridge.selection) {
                return window.WasmBridge.selection.marqueeHitTest(x0, y0, x1, y1);
            }
            return [];
        },

        // Vẽ trực tiếp overlay lên WebGL canvas bằng C++ Render
        renderOutlines() {
            if (!window.WasmBridge || !window.WasmBridge.selection) return;
            const { w, h } = this.getStageDimensions();

            if (this.selectedIndices.size === 0) {
                window.WasmBridge.selection.computeOutlines([]);
                window.WasmBridge.selection.clearOverlay(w, h);
                return;
            }

            const indices = Array.from(this.selectedIndices);
            window.WasmBridge.selection.computeOutlines(indices);

            const displayScale = this.getDisplayScale();
            // Constant screen line thickness of 2.0px regardless of zoom, pan, or resize
            const lineThick = (displayScale > 0.001) ? (2.0 / displayScale) : 2.0;

            window.WasmBridge.selection.renderOverlay(
                w, h, 0, 0, lineThick
            );
        },

        clearSelection() {
            this.selectedIndices.clear();
            this.isMarquee = false;
            this.hideMarqueeBox();
            if (window.WasmBridge && window.WasmBridge.selection) {
                window.WasmBridge.selection.computeOutlines([]);
                const { w, h } = this.getStageDimensions();
                window.WasmBridge.selection.clearOverlay(w, h);
            }
        },

        onFrameUpdate() {
            // Khi timeline đang phát hoặc tua khung hình, cập nhật lại viền theo C++ WebGL
            if (this.selectedIndices.size > 0) {
                this.renderOutlines();
            }
        },

        syncViewBox() {
            // Khi zoom / pan viewport thay đổi, cập nhật lại viền để độ dày đường kẻ luôn cố định 2.0px trên màn hình
            if (this.selectedIndices.size > 0) {
                this.renderOutlines();
            }
        },

        bindEvents() {
            const getStage = () => UI.fullscreenStage || document.getElementById('fullscreen-stage') || UI.viewport || document.getElementById('render-viewport');
            const stage = getStage();

            // 1. Nhận sự kiện mousedown trực tiếp trên stage (#fullscreen-stage hoặc #render-viewport)
            // Tuyệt đối không cho phép thao tác xuyên UI khi bấm vào header, tabs, resizer, hoặc các overlay drawers
            const handleStageMouseDown = (e) => {
                if (window.state && window.state.viewportTool !== 'pointer') return;
                if (window.tempSpaceHand) return;
                if (e.button !== 0) return; // Chỉ nhận chuột trái

                const currentStage = getStage();
                if (!currentStage) return;

                // Chặn thao tác nếu target là các thành phần UI (nút bấm, input, tabs, resizer, drawers)
                if (e.target.closest('button, input, select, textarea, .debug-inspector-panel, .debug-drawer-body, .debug-resizer, .drawer, .modal, .viewport-tool-btn, .tab-btn')) {
                    return;
                }

                // Chặn click xuyên UI nếu tại tọa độ chuột (clientX, clientY) có phần tử overlay nằm trên stage
                const topEl = document.elementFromPoint(e.clientX, e.clientY);
                if (topEl && !currentStage.contains(topEl)) {
                    return;
                }

                const currentVp = UI.viewport || document.getElementById('render-viewport');
                const vpRect = currentVp ? currentVp.getBoundingClientRect() : null;

                const isInsideViewport = vpRect ? (
                    e.clientX >= vpRect.left &&
                    e.clientX <= vpRect.right &&
                    e.clientY >= vpRect.top &&
                    e.clientY <= vpRect.bottom
                ) : false;

                const stagePos = this.clientToStage(e.clientX, e.clientY);

                if (isInsideViewport) {
                    const hitIdx = this.hitTest(stagePos.x, stagePos.y);

                    if (hitIdx !== -1) {
                        this.isMarquee = false;
                        if (e.shiftKey || e.ctrlKey || e.metaKey) {
                            if (this.selectedIndices.has(hitIdx)) {
                                this.selectedIndices.delete(hitIdx);
                            } else {
                                this.selectedIndices.add(hitIdx);
                            }
                        } else {
                            this.selectedIndices.clear();
                            this.selectedIndices.add(hitIdx);
                        }
                        this.renderOutlines();
                    } else {
                        // Click vào vùng trống bên trong viewport -> Unfocus xóa mọi line và bắt đầu marquee
                        this.clearSelection();
                        this.isMarquee = true;
                        this.marqueeStart = { x: stagePos.x, y: stagePos.y };
                        this.marqueeCurrent = { x: stagePos.x, y: stagePos.y };
                        this.marqueeClientStart = { x: e.clientX, y: e.clientY };
                        this.updateMarqueeBox(e.clientX, e.clientY, e.clientX, e.clientY);
                        this.renderOutlines();
                    }
                } else {
                    // Click vào nền cha letterbox stage (ngoài canvas viewport):
                    // Unfocus xóa mọi line và cho phép vẽ khoảng chọn marquee
                    this.clearSelection();
                    this.isMarquee = true;
                    this.marqueeStart = { x: stagePos.x, y: stagePos.y };
                    this.marqueeCurrent = { x: stagePos.x, y: stagePos.y };
                    this.marqueeClientStart = { x: e.clientX, y: e.clientY };
                    this.updateMarqueeBox(e.clientX, e.clientY, e.clientX, e.clientY);
                    this.renderOutlines();
                }

                e.preventDefault();
            };

            if (stage) {
                stage.addEventListener('mousedown', handleStageMouseDown);
            } else {
                // Fallback nếu stage gắn muộn
                document.addEventListener('DOMContentLoaded', () => {
                    const lateStage = getStage();
                    if (lateStage) lateStage.addEventListener('mousedown', handleStageMouseDown);
                });
            }

            // 2. Unfocus / Click outside: Lắng nghe trên document CHỈ để hủy chọn khi click ra ngoài
            // Tuyệt đối KHÔNG gọi hitTest, KHÔNG vẽ marquee, và KHÔNG preventDefault làm ảnh hưởng UI
            document.addEventListener('mousedown', (e) => {
                if (this.selectedIndices.size === 0) return;
                if (e.button !== 0) return;

                const currentStage = getStage();
                if (currentStage && currentStage.contains(e.target)) return;

                // Không hủy chọn khi click vào thanh debug inspector để user có thể kiểm tra Layers/Telemetry của layer đã chọn
                if (e.target.closest('#debug-inspector-panel, .debug-drawer-body, .debug-resizer')) {
                    return;
                }

                // Click ra bên ngoài stage vào các panel khác -> Hủy chọn gọn gàng
                this.clearSelection();
            });

            window.addEventListener('mousemove', (e) => {
                if (window.state && window.state.viewportTool !== 'pointer') return;
                if (!this.isMarquee) return; // Zero-allocation & 0% CPU khi không kéo marquee

                const stagePos = this.clientToStage(e.clientX, e.clientY);
                this.marqueeCurrent = { x: stagePos.x, y: stagePos.y };
                this.updateMarqueeBox(this.marqueeClientStart.x, this.marqueeClientStart.y, e.clientX, e.clientY);

                // Tìm các item va chạm với ô vuông marquee realtime qua C++
                const hits = this.marqueeHitTest(
                    this.marqueeStart.x, this.marqueeStart.y,
                    this.marqueeCurrent.x, this.marqueeCurrent.y
                );
                this.selectedIndices = new Set(hits);
                this.renderOutlines();
            });

            window.addEventListener('mouseup', () => {
                if (this.isMarquee) {
                    this.isMarquee = false;
                    this.hideMarqueeBox();
                    this.renderOutlines();
                }
            });

            // Khi mất focus cửa sổ (blur), tự động xóa sạch line (unfocus)
            window.addEventListener('blur', () => {
                this.clearSelection();
            });

            // Khi thay đổi kích thước cửa sổ, tự động vẽ lại outline với độ dày line cố định 2.0px
            window.addEventListener('resize', () => {
                if (this.selectedIndices.size > 0) {
                    this.renderOutlines();
                }
            });

            // Phím tắt Escape hủy chọn (unfocus)
            window.addEventListener('keydown', (e) => {
                const tag = (e.target && e.target.tagName) || '';
                const isTyping = tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'SELECT' || (e.target && e.target.isContentEditable);
                if (isTyping) return;

                if (e.key === 'Escape') {
                    this.clearSelection();
                }
            });
        }
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', () => SelectionTool.init());
    } else {
        SelectionTool.init();
    }

    window.SelectionTool = SelectionTool;

})(typeof window !== 'undefined' ? window : this);
