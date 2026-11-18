// ============================================================================
// CoreEngine - Dedicated Property Module (Arbitrary-Capacity Property System)
// ----------------------------------------------------------------------------
// [STOCKFISH DATA-STRUCTURE-CENTRIC ARCHITECTURE & BITBOARD SPECIFICATION]:
// 1. Unbounded Capacity (Khả năng bao hàm thuộc tính vô hạn):
//    - Không giới hạn ở 64-bit đơn lẻ.
//    - Sử dụng PropertyRegistry kết hợp PropertyBitmask (hỗ trợ BigInt vô hạn độ chính xác
//      và Multi-Word Bitset 256/512+ bit tương thích 1:1 với C++ WASM Memory).
// 2. Phân Tách 2 Bitmask Độc Lập:
//    - Defined Property Mask: Thuộc tính thực sự được thiết lập (loại bỏ giá trị mặc định).
//    - Keyframed Property Mask: Thuộc tính biến thiên qua keyframe theo thời gian.
// 3. Fast-Path Queries O(1):
//    - isStatic / isPureStatic: 0 ns hashing cho item tĩnh trong 60 FPS loop.
//    - isOpacityOnly: Bỏ qua biến đổi ma trận 4 đỉnh khi chỉ fade-in / fade-out.
//    - hasSpatialVariance: Kiểm tra nhanh biến động hình học.
// ============================================================================
(function (global) {
    'use strict';
    // ------------------------------------------------------------------------
    // 1. ĐỊNH NGHĨA ID CÁC THUỘC TÍNH TIÊU CHUẨN (STANDARD PROPERTY IDS 0..63)
    // ------------------------------------------------------------------------
    const PropertyId = Object.freeze({
        // Tier 0: Không gian & Hình học (0..7)
        LEFT:                0,
        TOP:                 1,
        WIDTH:               2,
        HEIGHT:              3,
        ROTATE:              4,
        SCALE:               5,
        TRANSFORM:           6,
        OPACITY:             7,
        // Tier 1: Typography & Phân cảnh văn bản (8..15)
        CONTENT:             8,
        FONT_SIZE:           9,
        FONT_FAMILY:         10,
        FONT_WEIGHT:         11,
        LINE_HEIGHT:         12,
        LETTER_SPACING:      13,
        TEXT_ALIGN:          14,
        COLOR:               15,
        // Tier 2: Viền, Nền & Hiệu ứng chữ (16..23)
        STROKE_WIDTH:        16,
        STROKE_COLOR:        17,
        BACKGROUND_COLOR:    18,
        BACKGROUND:          19,
        REVEAL_MODE:         20,
        REVEAL_DURATION:     21,
        REVEAL_STAGGER:      22,
        MASK_HOLE:           23,
        // Tier 3: Động lực học, Âm thanh & Layer Hierarchy (24..31)
        SPIN_SPEED:          24,
        VOLUME:              25,
        Z_INDEX:             26,
        FILTER:              27,
        BASS_ACTIVE:         28,
        TRIM_START:          29,
        TIER:                30,
        FLAGS:               31,
        // Tier 4: Timing & Cấu trúc thời gian (32..39)
        START:               32,
        END:                 33,
        DURATION:            34,
        CLASS_NAME:          35,
        SRC:                 36,
        TYPE:                37,
        EFFECT:              38,
        MUTED:               39,
        // Tier 5: Bố cục nâng cao (Flexbox, Grid & Box Model) (40..55)
        PADDING:             40,
        MARGIN:              41,
        DISPLAY:             42,
        FLEX_DIRECTION:      43,
        GAP:                 44,
        COLUMN_GAP:          45,
        ROW_GAP:             46,
        GRID_TEMPLATE_COLS:  47,
        GRID_TEMPLATE_ROWS:  48,
        BORDER:              49,
        BORDER_WIDTH:        50,
        BORDER_RADIUS:       51,
        BORDER_COLOR:        52,
        OVERFLOW:            53,
        ORIGIN_X:            54,
        ORIGIN_Y:            55,
        // Tier 6: Hiệu ứng Visual & Bóng đổ (56..63)
        SHADOW_COLOR:        56,
        SHADOW_BLUR:         57,
        SHADOW_OFFSET_X:     58,
        SHADOW_OFFSET_Y:     59,
        MASK_IMAGE:          60,
        BLEND_MODE:          61,
        BLUR:                62,
        PLAYBACK_RATE:       63,
        // Mốc thuộc tính chuẩn
        STANDARD_COUNT:      64
    });
    // ------------------------------------------------------------------------
    // 2. BỘ ĐĂNG KÝ THUỘC TÍNH ĐỘNG VÔ HẠN (UNBOUNDED PROPERTY REGISTRY)
    // ------------------------------------------------------------------------
    class PropertyRegistry {
        constructor() {
            this._nameToId = new Map();
            this._idToName = [];
            this._idToCategory = [];
            this._initStandardProperties();
        }
        _initStandardProperties() {
            const P = PropertyId;
            const reg = (id, name, cat, aliases = []) => {
                while (this._idToName.length < id) {
                    this._idToName.push('');
                    this._idToCategory.push('custom');
                }
                this._idToName[id] = name;
                this._idToCategory[id] = cat;
                this._registerKey(name, id);
                for (const alias of aliases) {
                    this._registerKey(alias, id);
                }
            };
            // Tier 0: Geometry
            reg(P.LEFT, 'left', 'geometry', ['x']);
            reg(P.TOP, 'top', 'geometry', ['y']);
            reg(P.WIDTH, 'width', 'geometry', ['w']);
            reg(P.HEIGHT, 'height', 'geometry', ['h']);
            reg(P.ROTATE, 'rotate', 'geometry', ['rotation', 'rotateDeg']);
            reg(P.SCALE, 'scale', 'geometry', ['zoom']);
            reg(P.TRANSFORM, 'transform', 'geometry');
            reg(P.OPACITY, 'opacity', 'geometry', ['alpha']);
            // Tier 1: Typography
            reg(P.CONTENT, 'content', 'typography', ['text', 'rawText', 'lyric', 'lyrics']);
            reg(P.FONT_SIZE, 'fontSize', 'typography', ['font-size']);
            reg(P.FONT_FAMILY, 'fontFamily', 'typography', ['font-family']);
            reg(P.FONT_WEIGHT, 'fontWeight', 'typography', ['font-weight']);
            reg(P.LINE_HEIGHT, 'lineHeight', 'typography', ['line-height']);
            reg(P.LETTER_SPACING, 'letterSpacing', 'typography', ['letter-spacing']);
            reg(P.TEXT_ALIGN, 'textAlign', 'typography', ['text-align']);
            reg(P.COLOR, 'color', 'typography', ['fill']);
            // Tier 2: Styles
            reg(P.STROKE_WIDTH, 'strokeWidth', 'style', ['stroke-width']);
            reg(P.STROKE_COLOR, 'strokeColor', 'style', ['stroke-color', 'stroke']);
            reg(P.BACKGROUND_COLOR, 'backgroundColor', 'style', ['background-color']);
            reg(P.BACKGROUND, 'background', 'style', ['bg']);
            reg(P.REVEAL_MODE, 'revealMode', 'animation', ['reveal-mode']);
            reg(P.REVEAL_DURATION, 'revealDuration', 'animation', ['reveal-duration']);
            reg(P.REVEAL_STAGGER, 'revealStagger', 'animation', ['reveal-stagger']);
            reg(P.MASK_HOLE, 'maskHole', 'style', ['mask-hole']);
            // Tier 3: Media & Audio Dynamics
            reg(P.SPIN_SPEED, 'spinSpeed', 'animation', ['spin-speed', 'spin']);
            reg(P.VOLUME, 'volume', 'audio', ['volunm', 'vol', 'level', 'gain']);
            reg(P.Z_INDEX, 'zIndex', 'geometry', ['z-index', 'z']);
            reg(P.FILTER, 'filter', 'style');
            reg(P.BASS_ACTIVE, 'bassActive', 'audio', ['bass-active']);
            reg(P.TRIM_START, 'trimStart', 'audio', ['trim-start', 'trim']);
            reg(P.TIER, 'tier', 'geometry');
            reg(P.FLAGS, 'flags', 'geometry');
            // Tier 4: Timing & Metadata
            reg(P.START, 'start', 'geometry');
            reg(P.END, 'end', 'geometry');
            reg(P.DURATION, 'duration', 'geometry', ['durationMs']);
            reg(P.CLASS_NAME, 'className', 'geometry', ['class']);
            reg(P.SRC, 'src', 'geometry', ['source', 'url']);
            reg(P.TYPE, 'type', 'geometry');
            reg(P.EFFECT, 'effect', 'geometry');
            reg(P.MUTED, 'muted', 'audio');
            // Tier 5: Layout & Flex/Grid
            reg(P.PADDING, 'padding', 'layout');
            reg(P.MARGIN, 'margin', 'layout');
            reg(P.DISPLAY, 'display', 'layout');
            reg(P.FLEX_DIRECTION, 'flexDirection', 'layout', ['flex-direction']);
            reg(P.GAP, 'gap', 'layout');
            reg(P.COLUMN_GAP, 'columnGap', 'layout', ['column-gap']);
            reg(P.ROW_GAP, 'rowGap', 'layout', ['row-gap']);
            reg(P.GRID_TEMPLATE_COLS, 'gridTemplateColumns', 'layout', ['grid-template-columns']);
            reg(P.GRID_TEMPLATE_ROWS, 'gridTemplateRows', 'layout', ['grid-template-rows']);
            reg(P.BORDER, 'border', 'style');
            reg(P.BORDER_WIDTH, 'borderWidth', 'style', ['border-width']);
            reg(P.BORDER_RADIUS, 'borderRadius', 'style', ['border-radius']);
            reg(P.BORDER_COLOR, 'borderColor', 'style', ['border-color']);
            reg(P.OVERFLOW, 'overflow', 'layout');
            reg(P.ORIGIN_X, 'originX', 'geometry', ['origin-x']);
            reg(P.ORIGIN_Y, 'originY', 'geometry', ['origin-y']);
            // Tier 6: Graphic Effects
            reg(P.SHADOW_COLOR, 'shadowColor', 'style', ['shadow-color']);
            reg(P.SHADOW_BLUR, 'shadowBlur', 'style', ['shadow-blur']);
            reg(P.SHADOW_OFFSET_X, 'shadowOffsetX', 'style', ['shadow-offset-x']);
            reg(P.SHADOW_OFFSET_Y, 'shadowOffsetY', 'style', ['shadow-offset-y']);
            reg(P.MASK_IMAGE, 'maskImage', 'style', ['mask-image']);
            reg(P.BLEND_MODE, 'blendMode', 'style', ['blend-mode']);
            reg(P.BLUR, 'blur', 'style');
            reg(P.PLAYBACK_RATE, 'playbackRate', 'audio', ['playback-rate']);
        }
        _registerKey(key, id) {
            const raw = String(key).trim();
            this._nameToId.set(raw, id);
            this._nameToId.set(raw.toLowerCase(), id);
            // Handle -- prefix for CSS vars
            if (raw.startsWith('--')) {
                const clean = raw.slice(2);
                this._nameToId.set(clean, id);
                this._nameToId.set(clean.toLowerCase(), id);
            }
            // Add camel <-> kebab variants
            const camel = this._toCamelCase(raw);
            if (camel !== raw) {
                this._nameToId.set(camel, id);
                this._nameToId.set(camel.toLowerCase(), id);
            }
            const kebab = this._toKebabCase(raw);
            if (kebab !== raw) {
                this._nameToId.set(kebab, id);
                this._nameToId.set(kebab.toLowerCase(), id);
            }
        }
        _toCamelCase(str) {
            return str.replace(/[-_]+(.)?/g, (_, c) => (c ? c.toUpperCase() : ''));
        }
        _toKebabCase(str) {
            return str.replace(/[A-Z]/g, m => '-' + m.toLowerCase()).replace(/^-/, '');
        }
        /**
         * Đăng ký tự động bất kỳ tên thuộc tính mới nào (không giới hạn số lượng).
         * Nếu đã có, trả về ID cũ; nếu chưa có, cấp phát ID mới từ 64 trở lên.
         */
        register(name, category = 'custom') {
            if (!name || typeof name !== 'string') return -1;
            const clean = name.trim();
            const existingId = this.getId(clean);
            if (existingId !== -1) return existingId;
            const newId = this._idToName.length;
            this._idToName.push(clean);
            this._idToCategory.push(category);
            this._registerKey(clean, newId);
            return newId;
        }
        /**
         * Tra cứu ID của thuộc tính theo tên. Trả về -1 nếu chưa đăng ký.
         */
        getId(name) {
            if (!name || typeof name !== 'string') return -1;
            const clean = name.trim();
            let id = this._nameToId.get(clean);
            if (id !== undefined) return id;
            id = this._nameToId.get(clean.toLowerCase());
            if (id !== undefined) return id;
            if (clean.startsWith('--')) {
                id = this._nameToId.get(clean.slice(2).toLowerCase());
                if (id !== undefined) return id;
            }
            return -1;
        }
        /**
         * Lấy tên chuẩn của thuộc tính theo ID.
         */
        getName(id) {
            if (typeof id !== 'number' || id < 0 || id >= this._idToName.length) return '';
            return this._idToName[id] || '';
        }
        /**
         * Lấy danh mục của thuộc tính theo ID.
         */
        getCategory(id) {
            if (typeof id !== 'number' || id < 0 || id >= this._idToCategory.length) return 'custom';
            return this._idToCategory[id] || 'custom';
        }
        /**
         * Tổng số lượng thuộc tính đã đăng ký trong hệ thống.
         */
        totalRegistered() {
            return this._idToName.length;
        }
    }
    const globalPropertyRegistry = new PropertyRegistry();
    // ------------------------------------------------------------------------
    // 3. CẤU TRÚC BITMASK ĐA TỪ VÔ HẠN (ARBITRARY-CAPACITY PROPERTY BITMASK)
    // ------------------------------------------------------------------------
    class PropertyBitmask {
        constructor(initial = 0n) {
            if (initial instanceof PropertyBitmask) {
                this._mask = initial._mask;
            } else if (typeof initial === 'bigint') {
                this._mask = initial;
            } else if (typeof initial === 'number') {
                this._mask = BigInt(initial);
            } else if (Array.isArray(initial) || ArrayBuffer.isView(initial)) {
                this._mask = PropertyBitmask.fromWords(initial)._mask;
            } else {
                this._mask = 0n;
            }
        }
        /**
         * Bật bit thuộc tính theo PropertyId (hỗ trợ bit từ 0 đến vô hạn).
         */
        set(propertyId) {
            if (typeof propertyId === 'number' && propertyId >= 0) {
                this._mask |= (1n << BigInt(propertyId));
            }
            return this;
        }
        /**
         * Tắt bit thuộc tính theo PropertyId.
         */
        clear(propertyId) {
            if (typeof propertyId === 'number' && propertyId >= 0) {
                this._mask &= ~(1n << BigInt(propertyId));
            }
            return this;
        }
        /**
         * Kiểm tra xem thuộc tính có được bật hay không.
         */
        test(propertyId) {
            if (typeof propertyId !== 'number' || propertyId < 0) return false;
            return (this._mask & (1n << BigInt(propertyId))) !== 0n;
        }
        /**
         * Trả về true nếu không có bất kỳ bit nào được bật (hoàn toàn tĩnh).
         */
        isEmpty() {
            return this._mask === 0n;
        }
        isStatic() {
            return this._mask === 0n;
        }
        /**
         * Kiểm tra fast-path: CHỈ DUY NHẤT thuộc tính OPACITY biến thiên.
         */
        isOpacityOnly() {
            return this._mask === (1n << BigInt(PropertyId.OPACITY));
        }
        /**
         * Kiểm tra xem có bất kỳ thuộc tính không gian nào (bits 0..6) biến thiên không.
         */
        hasSpatial() {
            return (this._mask & PropertyBitmask.MASK_SPATIAL_ALL) !== 0n;
        }
        /**
         * Đếm tổng số bit được bật trong mask (bất kể kích thước bao nhiêu bit).
         */
        popcount() {
            let count = 0;
            let v = this._mask;
            while (v > 0n) {
                v &= (v - 1n);
                count++;
            }
            return count;
        }
        /**
         * Lấy giá trị 64-bit thấp nhất (Word 0) phục vụ tương thích ngược.
         */
        low() {
            return this._mask & 0xFFFFFFFFFFFFFFFFn;
        }
        /**
         * Lấy từ 64-bit thứ wordIndex.
         */
        word(wordIndex = 0) {
            if (wordIndex < 0) return 0n;
            return (this._mask >> (BigInt(wordIndex) * 64n)) & 0xFFFFFFFFFFFFFFFFn;
        }
        /**
         * Xuất toàn bộ bitmask thành mảng BigUint64Array gồm numWords từ.
         */
        toWords(numWords = 4) {
            const arr = new BigUint64Array(numWords);
            for (let i = 0; i < numWords; i++) {
                arr[i] = (this._mask >> (BigInt(i) * 64n)) & 0xFFFFFFFFFFFFFFFFn;
            }
            return arr;
        }
        /**
         * Khôi phục bitmask từ mảng các từ 64-bit hoặc 32-bit.
         */
        static fromWords(words) {
            let mask = 0n;
            if (!words) return new PropertyBitmask(0n);
            if (words instanceof BigUint64Array || (Array.isArray(words) && typeof words[0] === 'bigint')) {
                for (let i = 0; i < words.length; i++) {
                    mask |= (BigInt(words[i]) & 0xFFFFFFFFFFFFFFFFn) << (BigInt(i) * 64n);
                }
            } else if (words instanceof Uint32Array || Array.isArray(words)) {
                for (let i = 0; i < words.length; i++) {
                    mask |= (BigInt(words[i]) & 0xFFFFFFFFn) << (BigInt(i) * 32n);
                }
            }
            return new PropertyBitmask(mask);
        }
        /**
         * Trả về danh sách tên các thuộc tính đang được bật trong mask này.
         */
        getPropertyNames() {
            const names = [];
            const total = globalPropertyRegistry.totalRegistered();
            for (let id = 0; id < total; id++) {
                if (this.test(id)) {
                    const name = globalPropertyRegistry.getName(id);
                    if (name) names.push(name);
                }
            }
            return names;
        }
        // Toán tử bitwise
        and(other) {
            const o = (other instanceof PropertyBitmask) ? other._mask : BigInt(other || 0n);
            return new PropertyBitmask(this._mask & o);
        }
        or(other) {
            const o = (other instanceof PropertyBitmask) ? other._mask : BigInt(other || 0n);
            return new PropertyBitmask(this._mask | o);
        }
        xor(other) {
            const o = (other instanceof PropertyBitmask) ? other._mask : BigInt(other || 0n);
            return new PropertyBitmask(this._mask ^ o);
        }
        not() {
            return new PropertyBitmask(~this._mask);
        }
        equals(other) {
            const o = (other instanceof PropertyBitmask) ? other._mask : BigInt(other || 0n);
            return this._mask === o;
        }
        toString(radix = 16) {
            return this._mask.toString(radix);
        }
        valueOf() {
            return this._mask;
        }
    }
    // Các compound mask tĩnh
    PropertyBitmask.MASK_SPATIAL_GEOM = (1n << BigInt(PropertyId.LEFT)) |
                                         (1n << BigInt(PropertyId.TOP)) |
                                         (1n << BigInt(PropertyId.WIDTH)) |
                                         (1n << BigInt(PropertyId.HEIGHT));
    PropertyBitmask.MASK_SPATIAL_AFFINE = (1n << BigInt(PropertyId.ROTATE)) |
                                           (1n << BigInt(PropertyId.SCALE)) |
                                           (1n << BigInt(PropertyId.TRANSFORM));
    PropertyBitmask.MASK_SPATIAL_ALL = PropertyBitmask.MASK_SPATIAL_GEOM | PropertyBitmask.MASK_SPATIAL_AFFINE;
    PropertyBitmask.MASK_VISUAL_STATE = PropertyBitmask.MASK_SPATIAL_ALL | (1n << BigInt(PropertyId.OPACITY));
    // ------------------------------------------------------------------------
    // 4. HÀM TÍNH TOÁN BITMASK THUỘC TÍNH (ARBITRARY COMPUTE ITEM MASKS)
    // ------------------------------------------------------------------------
    /**
     * Phân tích một timeline item bất kỳ và trả về bitmask chính xác
     * cho cả phần thuộc tính được khai báo (defined) và phần biến thiên qua keyframe.
     * Tự động đăng ký thuộc tính mới vào PropertyRegistry nếu chưa có.
     */
    function computeItemPropertyMasks(it) {
        if (!it || typeof it !== 'object') {
            const empty = new PropertyBitmask(0n);
            return {
                definedMask: empty,
                keyframedMask: empty,
                isStatic: true,
                isOpacityOnly: false,
                hasSpatial: false,
                definedProps: 0n,
                keyframedProps: 0n
            };
        }
        const definedMask = new PropertyBitmask(0n);
        const keyframedMask = new PropertyBitmask(0n);
        // 1. Quét toàn bộ trường tại root của item
        for (const k in it) {
            if (it[k] === undefined || it[k] === null) continue;
            if (k === 'css' || k === 'style' || k === 'params' || k === 'keyframes' || k === 'keyframe' || k === 'keyFrames') continue;
            let id = globalPropertyRegistry.getId(k);
            if (id === -1) {
                id = globalPropertyRegistry.register(k, 'custom');
            }
            if (id !== -1) {
                definedMask.set(id);
            }
        }
        // 2. Quét các trường con trong css, style, params
        const subContainers = [it.css, it.style, it.params];
        for (const sub of subContainers) {
            if (sub && typeof sub === 'object') {
                for (const k in sub) {
                    if (sub[k] === undefined || sub[k] === null) continue;
                    let id = globalPropertyRegistry.getId(k);
                    if (id === -1) {
                        id = globalPropertyRegistry.register(k, k.startsWith('--') ? 'CustomVariable' : 'custom');
                    }
                    if (id !== -1) {
                        definedMask.set(id);
                    }
                }
            }
        }
        // 3. Quét toàn bộ keyframes (hỗ trợ keyframes, keyframe, keyFrames, css.keyframes)
        const rawKfs = it.keyframes || it.keyframe || it.keyFrames ||
            (it.css && (it.css.keyframes || it.css.keyframe)) ||
            (it.style && (it.style.keyframes || it.style.keyframe));
        if (Array.isArray(rawKfs) && rawKfs.length > 0) {
            for (let i = 0; i < rawKfs.length; i++) {
                const kf = rawKfs[i];
                if (!kf || typeof kf !== 'object') continue;
                // Quét các trường trong keyframe
                for (const prop in kf) {
                    if (prop === 'offset' || prop === 'offsetMs' || prop === 'time' || prop === 't' || prop === 'at') continue;
                    if (prop === 'css' || prop === 'style') continue;
                    let id = globalPropertyRegistry.getId(prop);
                    if (id === -1) {
                        id = globalPropertyRegistry.register(prop, 'custom');
                    }
                    if (id !== -1 && kf[prop] !== undefined) {
                        keyframedMask.set(id);
                        definedMask.set(id);
                    }
                }
                // Quét kf.css / kf.style
                const kfSubs = [kf.css, kf.style];
                for (const sub of kfSubs) {
                    if (sub && typeof sub === 'object') {
                        for (const prop in sub) {
                            if (sub[prop] === undefined) continue;
                            let id = globalPropertyRegistry.getId(prop);
                            if (id === -1) {
                                id = globalPropertyRegistry.register(prop, prop.startsWith('--') ? 'CustomVariable' : 'custom');
                            }
                            if (id !== -1) {
                                keyframedMask.set(id);
                                definedMask.set(id);
                            }
                        }
                    }
                }
            }
        }
        const isStatic = keyframedMask.isEmpty();
        const isOpacityOnly = keyframedMask.isOpacityOnly();
        const hasSpatial = keyframedMask.hasSpatial();
        return {
            definedMask,
            keyframedMask,
            isStatic,
            isOpacityOnly,
            hasSpatial,
            definedProps: definedMask.low(),
            keyframedProps: keyframedMask.low()
        };
    }
    /**
     * Tự động tính và đính kèm PropertyBitmasks vào mảng các timeline items.
     */
    function attachPropertyMasks(items) {
        if (!Array.isArray(items)) return;
        for (let i = 0; i < items.length; i++) {
            const it = items[i];
            if (!it) continue;
            const res = computeItemPropertyMasks(it);
            it._definedMask = res.definedMask;
            it._keyframedMask = res.keyframedMask;
            it._definedProps = res.definedProps;
            it._keyframedProps = res.keyframedProps;
            it._isStatic = res.isStatic;
            it._isOpacityOnly = res.isOpacityOnly;
            it._hasSpatial = res.hasSpatial;
        }
    }
    // ------------------------------------------------------------------------
    // 5. XUẤT KHẨU MODUL TOÀN CỤC (GLOBAL & COMMONJS EXPORT)
    // ------------------------------------------------------------------------
    const PropertyModule = {
        PropertyId,
        PropertyRegistry: globalPropertyRegistry,
        PropertyBitmask,
        computeItemPropertyMasks,
        attachPropertyMasks,
        registerProperty: (name, cat) => globalPropertyRegistry.register(name, cat),
        getPropertyId: (name) => globalPropertyRegistry.getId(name),
        getPropertyName: (id) => globalPropertyRegistry.getName(id),
        totalProperties: () => globalPropertyRegistry.totalRegistered()
    };
    global.PropertyModule = PropertyModule;
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = PropertyModule;
    }
})(typeof window !== 'undefined' ? window : globalThis);
