// ============================================================================
// TEMPLATES — Handlebars Template Engine Manager
//   Quản lý, nạp, biên dịch và render các khuôn mẫu giao diện (.hbs)
//   Được lưu tập trung trong thư mục /templates/
// ============================================================================
(function (window) {
    'use strict';
    const _compiled = new Map();
    const _raw = new Map();
    // Micro fallback compiler nếu Handlebars chưa nạp kịp hoặc chạy offline
    function microCompile(templateStr) {
        return function (context) {
            context = context || {};
            let output = templateStr;
            // Xử lý {{#if var}}...{{else}}...{{/if}}
            output = output.replace(/\{\{#if\s+([\w\.]+)\}\}([\s\S]*?)(?:\{\{else\}\}([\s\S]*?))?\{\{\/if\}\}/g, (_, key, yesBlock, noBlock) => {
                const val = getVal(context, key);
                return val ? yesBlock : (noBlock || '');
            });
            // Xử lý {{#unless var}}...{{/unless}}
            output = output.replace(/\{\{#unless\s+([\w\.]+)\}\}([\s\S]*?)\{\{\/unless\}\}/g, (_, key, block) => {
                const val = getVal(context, key);
                return !val ? block : '';
            });
            // Xử lý {{var}} và {{{var}}}
            output = output.replace(/\{\{\{?([\w\.]+)\}?\}\}/g, (_, key) => {
                const val = getVal(context, key);
                return val !== undefined && val !== null ? val : '';
            });
            return output;
        };
    }
    function getVal(obj, path) {
        if (!obj) return undefined;
        const parts = path.split('.');
        let cur = obj;
        for (const p of parts) {
            if (cur == null) return undefined;
            cur = cur[p];
        }
        return cur;
    }
    function compileTemplate(source) {
        if (window.Handlebars && typeof window.Handlebars.compile === 'function') {
            try {
                return window.Handlebars.compile(source);
            } catch (e) {
                console.warn('[Templates] Lỗi biên dịch Handlebars, chuyển sang micro-compiler:', e);
            }
        }
        return microCompile(source);
    }
    const Templates = {
        // Đăng ký template với tên và mã nguồn
        register(name, source) {
            _raw.set(name, source);
            _compiled.set(name, compileTemplate(source));
        },
        // Render ra chuỗi HTML
        render(name, data) {
            let fn = _compiled.get(name);
            if (!fn) {
                const raw = _raw.get(name);
                if (raw) {
                    fn = compileTemplate(raw);
                    _compiled.set(name, fn);
                } else {
                    console.warn(`[Templates] Chưa đăng ký template: "${name}"`);
                    return '';
                }
            }
            try {
                return fn(data || {});
            } catch (e) {
                console.error(`[Templates] Lỗi khi render template "${name}":`, e);
                return '';
            }
        },
        // Render ra DOM Element trực tiếp
        renderElement(name, data) {
            const html = this.render(name, data).trim();
            const temp = document.createElement('div');
            temp.innerHTML = html;
            return temp.firstElementChild || temp;
        },
        // Nạp file .hbs từ thư mục templates/
        async loadFile(name, filePath) {
            try {
                const res = await fetch(filePath);
                if (res.ok) {
                    const text = await res.text();
                    this.register(name, text);
                    return true;
                }
            } catch (e) {
                // Bỏ qua nếu chạy dưới protocol file://
            }
            return false;
        },
        // Nạp lại tất cả các file .hbs từ thư mục templates/
        async loadAll() {
            const files = [
                ['media-card', 'templates/media-card.hbs'],
                ['media-audio-item', 'templates/media-audio-item.hbs'],
                ['font-item', 'templates/font-item.hbs'],
                ['effect-item', 'templates/effect-item.hbs'],
                ['spec-item', 'templates/spec-item.hbs']
            ];
            await Promise.allSettled(files.map(([name, path]) => this.loadFile(name, path)));
        }
    };
    // Đăng ký trước các template mặc định (để nạp và render ngay lập tức khi mở app / reload, 0ms latency)
    Templates.register('media-card', `<div class="media-card" data-path="{{path}}" title="{{name}}{{#if durationText}} ({{durationText}}){{/if}}&#10;Click để thêm vào Timeline">
    <div class="media-card-thumb-wrap">
        {{#if isVideo}}
        <div class="media-placeholder-icon" style="{{#if hasThumb}}display:none;{{/if}}">
            <span class="media-placeholder-tag">VIDEO</span>
        </div>
        <img class="media-card-thumb-img" src="{{thumb}}" alt="{{name}}" loading="lazy" style="{{#unless hasThumb}}display:none;{{/unless}}">
        <span class="media-card-badge" style="{{#unless durationText}}display:none;{{/unless}}">{{durationText}}</span>
        {{else}}
        <img class="media-card-thumb-img" src="{{src}}" alt="{{name}}" loading="lazy">
        {{/if}}
    </div>
    <div class="media-card-info">
        <div class="media-card-name" title="{{name}}">{{name}}</div>
    </div>
</div>`);
    Templates.register('media-audio-item', `<div class="media-audio-item" data-path="{{path}}" title="Click để thêm &quot;{{displayName}}&quot; vào Timeline">
    <button type="button" class="media-audio-play-btn" title="Nghe thử">▶</button>
    <div class="media-audio-col-name" title="{{displayName}}">{{displayName}}</div>
    <div class="media-audio-col-type" title="Định dạng: {{audioType}}">{{audioType}}</div>
    <div class="media-audio-col-artist" title="Tác giả: {{artist}}">{{artist}}</div>
    <div class="media-audio-col-path" title="Đường dẫn / Link: {{pathDisplay}}">{{pathDisplay}}</div>
    <div class="media-audio-col-duration" title="Thời lượng">{{durationText}}</div>
</div>`);
    Templates.register('font-item', `<div class="media-audio-item media-font-item" data-family="{{family}}" title="Click để áp dụng phông &quot;{{family}}&quot; vào Timeline">
    <div class="media-audio-col-name" style="font-family: '{{family}}', sans-serif;" title="{{family}}">{{family}}</div>
    <div class="media-audio-col-path" title="Đường dẫn: {{filePath}}">{{filePath}}</div>
</div>`);
    Templates.register('effect-item', `<div class="effect-item" data-effect-id="{{id}}" title="{{name}} ({{tag}}) - {{desc}}">
    <div class="effect-item-header">
        <span class="effect-item-title">{{name}}</span>
        <span class="effect-item-tag">{{tag}}</span>
    </div>
    <div class="effect-item-desc">{{desc}}</div>
</div>`);
    Templates.register('spec-item', `<div class="export-spec-item" data-spec="{{key}}">
    <span class="export-spec-label">{{label}}:</span>
    <span class="export-spec-value" id="{{id}}">{{value}}</span>
</div>`);
    // Tự động nạp cập nhật từ các file .hbs trong thư mục templates/ nếu đang chạy trên HTTP/HTTPS
    if (typeof window !== 'undefined' && window.location && window.location.protocol.startsWith('http')) {
        Templates.loadAll().catch(() => {});
    }
    window.Templates = Templates;
})(window);
