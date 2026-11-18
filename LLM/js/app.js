const $ = id => document.getElementById(id);
const enc = new TextEncoder(), dec = new TextDecoder();
const copyFeedbackTimers = new WeakMap();
const RAM_BUDGET_MB = 128;           // vượt ngưỡng này => chế độ PAGED
const STORE_KEY = 'bitllm-chat-v1', MAX_STORE = 500;
let M = null, modelReady = false, loading = false, busy = false, editingMessageId = null;

createBitLLMModule().then(m => {
    M = m;
    if (!M.HEAPU8) {
        $('badge').textContent = '· thiếu HEAPU8: build lại với -sEXPORTED_RUNTIME_METHODS=HEAPU8';
        $('badge').className = 'badge er';
        return;
    }
    M._wasm_bit_llm_init();
    $('badge').textContent = '· WASM sẵn sàng — hãy nạp tệp .bin';
    $('badge').className = 'badge ok';
    syncControls();
    refreshFeedbackButtons();
    $('q').focus();
});

const heap = () => M.HEAPU8;
const tokenStr = id => {
    const p = M._wasm_bit_llm_get_token_ptr(id), n = M._wasm_bit_llm_get_token_len(id);
    return dec.decode(heap().subarray(p, p + n));
};
const info = (cls, msg) => { const e = $('wInfo'); e.className = 'msg ' + cls; e.textContent = msg; };
const mb = n => (n / 1024 / 1024).toFixed(1);

function syncControls() {
    $('ask').disabled = !M || busy || loading || !!editingMessageId;
    $('q').disabled = busy || loading || !!editingMessageId;
    $('wUp').disabled = !M || busy || loading;
    if (typeof list !== 'undefined') list.querySelectorAll('.edit-button').forEach(button => {
        button.disabled = busy || loading;
    });
}

/* =====================  NẠP MÔ HÌNH (streaming, giữ nguyên logic cũ)  ===================== */
async function lazyLoadBinFile(file) {
    if (!M || loading || busy) return;
    loading = true; modelReady = false; syncControls(); refreshFeedbackButtons();
    $('wBar').style.width = '0';
    info('wa', '· Đang chuẩn bị nạp tệp…');
    try {
        const headerSize = M._wasm_bit_llm_get_header_size();
        const maxChunk = M._wasm_bit_llm_get_chunk_buffer_size();
        M._wasm_bit_llm_set_ram_budget_kb(RAM_BUDGET_MB * 1024);
        const total = file.size;
        if (total < headerSize) throw new Error(`file nhỏ không hợp lệ.`);

        const beginResult = M._wasm_bit_llm_begin_load();

        if (beginResult !== headerSize) {
            throw new Error(
                `begin_load thất bại: trả về ${beginResult}, header=${headerSize}`
            );
        }

        const bufPtr = M._wasm_bit_llm_get_chunk_buffer_ptr();

        if (!bufPtr || !maxChunk || maxChunk <= 0) {
            throw new Error(
                `Chunk buffer không hợp lệ: ptr=${bufPtr}, chunk=${maxChunk}`
            );
        }

        let offset = 0, target = headerSize, lastUi = 0;

        const push = async (u8) => {
            for (let s = 0; s < u8.length && offset < target; s += maxChunk) {
                const part = u8.subarray(s, Math.min(s + maxChunk, u8.length, s + (target - offset)));
                heap().set(part, bufPtr);
                if (!M._wasm_bit_llm_load_chunk_direct(offset, part.length)) {
                    if (offset < headerSize) {
                        const code = M._wasm_bit_llm_get_header_error?.() ?? -1;
                        const reasons = {
                            1: 'Sai MODEL_MAGIC',
                            2: 'Sai MODEL_VERSION',
                            3: 'Sai ARCH_HASH: trainer và inference khác cấu hình',
                            4: 'Vocab count không hợp lệ',
                            5: 'Thông số kiến trúc không khớp',
                            6: 'Arena/descriptor/document không hợp lệ',
                            7: 'Format flags không hợp lệ',
                            8: 'Temperature không hợp lệ'
                        };
                        throw new Error(
                            `Header lỗi mã ${code}: ${reasons[code] || 'Không xác định'}`
                        );
                    }
                    throw new Error('Dữ liệu trọng số bị lỗi tại byte ' + offset);
                }
                offset += part.length;
                if (target === headerSize && offset === headerSize) {
                    target = M._wasm_bit_llm_get_stream_bytes();
                    const fsz = M._wasm_bit_llm_get_file_size();

                    if (!Number.isFinite(target) || target <= headerSize) {
                        throw new Error(`Header không hợp lệ: streamBytes=${target}`);
                    }

                    if (!Number.isFinite(fsz) || fsz !== target) {
                        throw new Error(`Kích thước layout không hợp lệ: file=${fsz}, stream=${target}`);
                    }

                    if (total !== target) {
                        throw new Error(
                            `Sai kích thước BIN: file=${total}, model=${target}, vocab=${M._wasm_bit_llm_get_vocab_count()}`
                        );
                    }
                }
            }
            const now = performance.now();
            if (now - lastUi > 80) {
                lastUi = now;
                $('wBar').style.width = Math.round(offset / (target || 1) * 100) + '%';
                info('wa', `· Đang nạp: ${mb(offset)}/${mb(target)} MB`);
                await new Promise(r => setTimeout(r));
            }
        };

        await push(new Uint8Array(await file.slice(0, headerSize).arrayBuffer()));
        for (let s = headerSize; s < target; s += maxChunk)
            await push(new Uint8Array(await file.slice(s, Math.min(s + maxChunk, target)).arrayBuffer()));

        // PAGED cần đọc khối đồng bộ: chỉ chạy được trong Web Worker với tệp cục bộ
        if (M._wasm_bit_llm_is_paged()) {
            const inWorker = typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope;
            if (!inWorker)
                throw new Error(`Mô hình ${mb(M._wasm_bit_llm_get_file_size())} MB vượt ngân sách RAM ${RAM_BUDGET_MB}.`);
            const sync = new FileReaderSync();
            M.bitLLMReadBlock = (blk, dst, bytes, off) => {
                const buf = sync.readAsArrayBuffer(file.slice(off, off + bytes));
                heap().set(new Uint8Array(buf), dst);
                return buf.byteLength === bytes;
            };
        }

        info('wa', '· Đang kiểm tra mô hình…');
        await new Promise(r => setTimeout(r, 10));
        if (!M._wasm_bit_llm_finalize_load()) throw new Error('Mô hình không hợp lệ.');

        const vc = M._wasm_bit_llm_get_vocab_count();
        const mode = M._wasm_bit_llm_is_paged() ? `PAGED (pool ${M._wasm_bit_llm_get_max_slots()} khối)` : 'FLAT';
        modelReady = true;
        $('wBar').style.width = '100%';
        info('ok', `${vc} token · ${mode} · RAM ≈ ${mb(M._wasm_bit_llm_get_ram_bytes())} MB`);
        $('badge').textContent = `· ${file.name} đã sẵn sàng`;
        $('badge').className = 'badge ok';
        addMessage('system', `Đã nạp ${file.name} ${vc} token.`);
        refreshFeedbackButtons();
        document.body.classList.remove('side-open');
    } catch (e) {
        M._wasm_bit_llm_init();
        modelReady = false;
        refreshFeedbackButtons();
        $('wBar').style.width = '0';
        info('er', `Lỗi nạp: ${e.message}`);
    } finally {
        loading = false; $('wFile').value = ''; syncControls();
    }
}

$('wUp').onclick = () => $('wFile').click();
$('wFile').onchange = e => { const f = e.target.files[0]; if (f) lazyLoadBinFile(f); };
const dz = $('dropzone');
['dragenter', 'dragover'].forEach(ev => dz.addEventListener(ev, e => { e.preventDefault(); dz.classList.add('drag'); }));
['dragleave', 'drop'].forEach(ev => dz.addEventListener(ev, e => { e.preventDefault(); dz.classList.remove('drag'); }));
dz.addEventListener('drop', e => { const f = e.dataTransfer.files[0]; if (f) lazyLoadBinFile(f); });

/* =====================  SUY LUẬN  ===================== */
function infer(text) {
    const b = enc.encode(text);
    heap().set(b, M._wasm_bit_llm_get_input_ptr());
    const t0 = performance.now(), n = M._wasm_bit_llm_infer(b.length), ms = performance.now() - t0, op = M._wasm_bit_llm_get_output_ptr();
    return { out: dec.decode(heap().subarray(op, op + n)), ms, views: 1, votes: 1,
        feedbackReady: [1, 2].includes(M._wasm_bit_llm_get_answer_source?.()) };
}

// Lượt 0: beam tất định; các lượt sau: Gumbel-top-k. Dừng sớm (sau minMs) khi lõi báo đủ tự tin (Wilson).
async function think(text, minMs, maxMs) {
    const b = enc.encode(text);
    heap().set(b, M._wasm_bit_llm_get_input_ptr());
    if (!M._wasm_bit_llm_think_begin(b.length)) return { out: '', ms: 0, views: 0, votes: 0 };
    const src = M._wasm_bit_llm_get_answer_source();
    if (src === 1 || src === 3) {            // retrieval có sẵn; source 3 chỉ xảy ra khi model chưa thể chạy
        M._wasm_bit_llm_think_step();
        const n = M._wasm_bit_llm_think_finish(), op = M._wasm_bit_llm_get_output_ptr();
        return { out: dec.decode(heap().subarray(op, op + n)), ms: 0, views: 1, votes: 1,
            feedbackReady: src === 1 && [1, 2].includes(M._wasm_bit_llm_get_answer_source?.()) };
    }
    const t0 = performance.now();
    let views = 0, last = t0;
    while (true) {
        views = M._wasm_bit_llm_think_step();
        const el = performance.now() - t0, lead = M._wasm_bit_llm_think_leader_votes();
        if (el >= maxMs) break;
        if (el >= minMs && M._wasm_bit_llm_think_confident()) break;
        if (performance.now() - last > 100) {
            $('ans').textContent = `Đang suy nghĩ… ${views} lượt · đồng thuận ${lead}/${views} · ${(el / 1000).toFixed(1)}s`;
            await new Promise(r => setTimeout(r));
            last = performance.now();
        }
    }
    const n = M._wasm_bit_llm_think_finish(), op = M._wasm_bit_llm_get_output_ptr();
    return { out: dec.decode(heap().subarray(op, op + n)), ms: performance.now() - t0, views, votes: M._wasm_bit_llm_get_stat(7),
        feedbackReady: [1, 2].includes(M._wasm_bit_llm_get_answer_source?.()) };
}

async function runInference(text) {
    if (!modelReady || !M) throw new Error('Hãy nạp mô hình trước khi suy luận.');
    const num = (id, fallback) => {
        const value = parseFloat($(id).value);
        return Number.isFinite(value) ? value : fallback;
    };
    M._wasm_bit_llm_set_beam(Math.max(1, Math.min(8, Math.round(num('beam', 4)))));
    if (M._wasm_bit_llm_set_creativity)
        M._wasm_bit_llm_set_creativity($('creative') ? Math.max(0, Math.min(300, Math.round(num('creative', 100)))) : 100);
    const minSeconds = Math.max(0, num('tmin', 2));
    const maxSeconds = Math.max(minSeconds, num('tmax', 6));
    return maxSeconds <= 0 ? infer(text) : await think(text, minSeconds * 1000, maxSeconds * 1000);
}

async function runFreshInference(text, previousAnswer = '') {
    if (!modelReady || !M) throw new Error('Hãy nạp mô hình trước khi suy luận.');
    const num = (id, fallback) => {
        const value = parseFloat($(id).value);
        return Number.isFinite(value) ? value : fallback;
    };
    const baseBeam = Math.max(1, Math.min(8, Math.round(num('beam', 4))));
    const baseCreativity = $('creative') ? Math.max(0, Math.min(300, Math.round(num('creative', 100)))) : 100;
    const minMs = Math.max(1200, Math.max(0, num('tmin', 2)) * 1000);
    const maxMs = Math.max(minMs + 2000, Math.max(0, num('tmax', 6)) * 1000, 4000);
    const retrieval = M._wasm_bit_llm_get_retrieval?.();
    if (M._wasm_bit_llm_set_retrieval) M._wasm_bit_llm_set_retrieval(0);
    try {
        const old = previousAnswer.trim().replace(/\s+/g, ' ').toLocaleLowerCase();
        let result = null;
        for (let attempt = 0; attempt < 3; ++attempt) {
            const beam = attempt === 0 ? baseBeam : 1 + ((baseBeam - 1 + attempt) % 8);
            const creativity = attempt === 0 ? baseCreativity : Math.max(0, Math.min(300, baseCreativity + (attempt === 1 ? 80 : -80)));
            M._wasm_bit_llm_set_beam(beam);
            if (M._wasm_bit_llm_set_creativity) M._wasm_bit_llm_set_creativity(creativity);
            if (M._wasm_bit_llm_set_search_seed) {
                const seed = new Uint32Array(1);
                if (globalThis.crypto?.getRandomValues) globalThis.crypto.getRandomValues(seed);
                else seed[0] = (Date.now() ^ Math.floor(Math.random() * 0xFFFFFFFF)) >>> 0;
                M._wasm_bit_llm_set_search_seed(seed[0]);
            }
            result = await think(text, minMs, maxMs);
            const fresh = result.out.trim().replace(/\s+/g, ' ').toLocaleLowerCase();
            if (!old || fresh !== old) return result;
        }
        if (old && result?.out.trim().replace(/\s+/g, ' ').toLocaleLowerCase() === old)
            throw new Error('Mô hình vẫn lặp lại câu trả lời cũ sau 3 lượt tìm kiếm.');
        return result;
    } finally {
        M._wasm_bit_llm_set_beam(baseBeam);
        if (M._wasm_bit_llm_set_creativity) M._wasm_bit_llm_set_creativity(baseCreativity);
        if (M._wasm_bit_llm_set_retrieval)
            M._wasm_bit_llm_set_retrieval(Number.isFinite(retrieval) ? retrieval : 450);
    }
}

/* ===================== CHAT ĐA TRANG (nhiều cuộc trò chuyện độc lập) ===================== */
const list = $('message-list');
const INITIAL_RENDER_COUNT = 40;
const OLDER_BATCH_SIZE = 30;
const STORE_KEY_V2 = 'bitllm-chats-v2';   // {activeId, chats:[{id,title,createdAt,updatedAt,messages[]}]}
const MAX_CHATS = 50, DAY_MS = 24 * 60 * 60 * 1000;
const WEEKDAYS = ['Chủ nhật', 'Thứ hai', 'Thứ ba', 'Thứ tư', 'Thứ năm', 'Thứ sáu', 'Thứ bảy'];

let store = { activeId: null, chats: [] };
let messages = [];          // = messages của cuộc trò chuyện đang mở (cùng tham chiếu với chat.messages)
let renderedStart = 0;      // chỉ số đầu tiên đang có trong DOM
let typingEl = null;
let loadingOlder = false;

const el = (tag, cls, text) => {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text != null) node.textContent = text;
    return node;
};

/* ---------- Thời gian: luôn tính lại theo "bây giờ" ---------- */
const pad2 = n => String(n).padStart(2, '0');
const startOfDay = d => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
// Số ngày lịch giữa ts và hôm nay (0 = hôm nay, 1 = hôm qua...)
const dayDiff = ts => Math.round((startOfDay(new Date()) - startOfDay(new Date(ts))) / DAY_MS);

function fmtDay(ts) {
    const diff = dayDiff(ts), d = new Date(ts);
    if (diff <= 0) return 'Hôm nay';
    if (diff === 1) return 'Hôm qua';
    if (diff < 7) return WEEKDAYS[d.getDay()];                       // trong tuần qua: "Thứ ba"
    return `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}/${d.getFullYear()}`; // cũ hơn: dd/mm/yyyy
}
function fmtTime(ts) {
    const d = new Date(ts);
    return d.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });
}

// Cập nhật lại nhãn khi mở lại tab / qua nửa đêm / trang mở lâu
function refreshTimeLabels() {
    list.querySelectorAll('.message-time[data-ts]').forEach(n => {
        const t = fmtTime(+n.dataset.ts);
        if (n.textContent !== t) n.textContent = t;
    });
    renderChatList();
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) refreshTimeLabels(); });
window.addEventListener('focus', refreshTimeLabels);
setInterval(refreshTimeLabels, 60 * 1000);

/* ---------- Lưu trữ nhiều cuộc trò chuyện ---------- */
const newId = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
const hasContent = msgs => msgs.some(m => m.sender !== 'system');
const activeChat = () => store.chats.find(c => c.id === store.activeId);

const sanitizeMessages = arr => {
    if (!Array.isArray(arr)) return [];
    const valid = arr.filter(m => m && typeof m.text === 'string' &&
        ['me', 'other', 'system'].includes(m.sender) && Number.isFinite(m.createdAt)).slice(-MAX_STORE);
    let latestQuestion = '';
    return valid.map(m => {
        if (m.sender === 'me') latestQuestion = m.text;
        const question = m.sender === 'other'
            ? (typeof m.question === 'string' && m.question ? m.question : latestQuestion)
            : m.question;
        const voted = m.feedback === 1 || m.feedback === -1;
        const savedButtons = m.buttons && typeof m.buttons === 'object' ? m.buttons : {};
        const buttonState = m.sender === 'other' ? {
            like: { isEnable: true },
            dislike: { isEnable: true },
            regenerate: { isEnable: typeof savedButtons.regenerate?.isEnable === 'boolean' ? savedButtons.regenerate.isEnable : !!question },
            copy: { isEnable: typeof savedButtons.copy?.isEnable === 'boolean' ? savedButtons.copy.isEnable : true },
            more: { isEnable: typeof savedButtons.more?.isEnable === 'boolean' ? savedButtons.more.isEnable : true }
        } : m.sender === 'me' ? {
            copy: { isEnable: typeof savedButtons.copy?.isEnable === 'boolean' ? savedButtons.copy.isEnable : true }
        } : undefined;
        return { ...m, ...(m.sender === 'other' ? { question } : {}),
            ...(buttonState ? { buttons: buttonState } : {}),
            feedback: voted ? m.feedback : 0,
            feedbackReady: m.sender === 'other' && !!question };
    });
};

function deriveTitle(msgs) {
    const first = msgs.find(m => m.sender === 'me');
    if (!first) return '';
    const t = first.text.replace(/\s+/g, ' ').trim();
    return t.length > 40 ? t.slice(0, 40) + '…' : t;
}

function makeChat(msgs = []) {
    const now = Date.now();
    return {
        id: newId(), title: deriveTitle(msgs), createdAt: msgs.length ? msgs[0].createdAt : now,
        updatedAt: msgs.length ? msgs[msgs.length - 1].createdAt : now, messages: msgs
    };
}

function loadStore() {
    let loaded = false, migrated = false;
    try {
        const p = JSON.parse(localStorage.getItem(STORE_KEY_V2) || 'null');
        if (p && Array.isArray(p.chats)) {
            store.chats = p.chats.filter(c => c && typeof c.id === 'string').slice(0, MAX_CHATS).map(c => {
                const msgs = sanitizeMessages(c.messages);
                return {
                    id: c.id,
                    title: typeof c.title === 'string' ? c.title : deriveTitle(msgs),
                    createdAt: Number.isFinite(c.createdAt) ? c.createdAt : Date.now(),
                    updatedAt: Number.isFinite(c.updatedAt) ? c.updatedAt : Date.now(),
                    messages: msgs
                };
            });
            store.activeId = p.activeId;
            loaded = true;
        }
    } catch { /* dữ liệu hỏng => bỏ qua */ }

    if (!loaded) {   // chuyển lịch sử 1 trang cũ (v1) thành cuộc trò chuyện đầu tiên
        try {
            const old = sanitizeMessages(JSON.parse(localStorage.getItem(STORE_KEY) || '[]'));
            if (old.length) { const c = makeChat(old); store.chats = [c]; store.activeId = c.id; migrated = true; }
        } catch { }
    }
    // bỏ các trang rỗng không phải trang đang mở
    store.chats = store.chats.filter(c => c.id === store.activeId || hasContent(c.messages));
    if (!activeChat()) {
        const c = makeChat();
        store.chats.push(c);
        store.activeId = c.id;
    }
    messages = activeChat().messages;
    if (migrated) { try { localStorage.removeItem(STORE_KEY); } catch { } }
    saveStore(); // persists normalized per-button isEnable flags for legacy conversations
}

function saveStore() {
    const build = () => JSON.stringify({
        activeId: store.activeId,
        chats: store.chats.map(c => ({ ...c, messages: c.messages.slice(-MAX_STORE) }))
    });
    for (let tries = 0; tries < 20; tries++) {
        try { localStorage.setItem(STORE_KEY_V2, build()); return; }
        catch (err) {
            // Đầy dung lượng: bỏ trang cũ nhất (không phải trang đang mở) rồi thử lại
            const victims = store.chats.filter(c => c.id !== store.activeId).sort((a, b) => a.updatedAt - b.updatedAt);
            if (!victims.length) { console.warn('Không thể lưu lịch sử chat:', err); return; }
            store.chats = store.chats.filter(c => c !== victims[0]);
        }
    }
}

/* ---------- Danh sách cuộc trò chuyện: khung nằm sẵn trong HTML (#chatList > #chatItems), JS nạp item lười theo lô ---------- */
const CHAT_BATCH = 20;      // số item nạp mỗi lần
let chatSorted = [], chatShown = CHAT_BATCH;

function ensureChatListEl() {
    let box = $('chatList');
    if (!box) {   // dự phòng nếu HTML chưa có khung
        box = el('nav', 'chat-list');
        box.id = 'chatList';
        $('newChat').insertAdjacentElement('afterend', box);
    }
    let items = $('chatItems');
    if (!items) {
        if (!box.querySelector('.card-header')) box.appendChild(el('div', 'card-header', 'Cuộc trò chuyện'));
        items = el('div', 'chat-items');
        items.id = 'chatItems';
        box.appendChild(items);
    }
    if (!box.dataset.bound) {
        box.dataset.bound = '1';
        box.addEventListener('click', e => {
            const del = e.target.closest('[data-del]');
            if (del) { e.stopPropagation(); deleteChat(del.dataset.del); return; }
            const item = e.target.closest('.chat-item');
            if (item) switchChat(item.dataset.id);
        });
        box.addEventListener('keydown', e => {
            if (e.key !== 'Enter' && e.key !== ' ') return;
            const item = e.target.closest('.chat-item');
            if (item && e.target === item) { e.preventDefault(); switchChat(item.dataset.id); }
        });
        // Lười: cuộn gần đáy danh sách thì nạp thêm lô tiếp theo
        box.addEventListener('scroll', () => {
            if (box.scrollTop + box.clientHeight >= box.scrollHeight - 40) loadMoreChats();
        }, { passive: true });
    }
    return items;
}

function makeChatItem(c) {
    const item = el('div', 'chat-item' + (c.id === store.activeId ? ' active' : ''));
    item.dataset.id = c.id;
    item.setAttribute('role', 'button');
    item.tabIndex = 0;
    const txt = el('div', 'chat-item-text');
    txt.appendChild(el('span', 'chat-item-title', c.title || 'Cuộc trò chuyện mới'));
    txt.appendChild(el('small', 'chat-item-time', fmtDay(c.updatedAt)));
    const del = el('button', 'chat-item-del', '✕');
    del.type = 'button';
    del.title = 'Xoá cuộc trò chuyện';
    del.setAttribute('aria-label', 'Xoá cuộc trò chuyện');
    del.dataset.del = c.id;
    item.append(txt, del);
    return item;
}

function renderChatList() {
    const items = ensureChatListEl();
    chatSorted = [...store.chats].sort((a, b) => b.updatedAt - a.updatedAt);
    const ai = chatSorted.findIndex(c => c.id === store.activeId);
    if (ai >= chatShown) chatShown = ai + 1;            // luôn hiện trang đang mở
    const frag = document.createDocumentFragment();
    for (const c of chatSorted.slice(0, chatShown)) frag.appendChild(makeChatItem(c));
    items.replaceChildren(frag);
}

function loadMoreChats() {
    const items = $('chatItems');
    if (!items) return;
    const from = items.childElementCount;
    if (from >= chatSorted.length) return;
    const to = Math.min(chatSorted.length, from + CHAT_BATCH);
    const frag = document.createDocumentFragment();
    for (let i = from; i < to; i++) frag.appendChild(makeChatItem(chatSorted[i]));
    items.appendChild(frag);
    chatShown = to;
}

function openChat(chat) {
    store.activeId = chat.id;
    messages = chat.messages;
    saveStore();
    renderInitialHistory();
    renderChatList();
    document.body.classList.remove('side-open');
    $('q').focus();
}

function switchChat(id) {
    if (busy || id === store.activeId) { document.body.classList.remove('side-open'); return; }
    const c = store.chats.find(x => x.id === id);
    if (c) openChat(c);
}

function createChat() {
    if (busy) return;
    if (!hasContent(messages)) { document.body.classList.remove('side-open'); $('q').focus(); return; } // đang ở trang trống
    const c = makeChat();
    store.chats.push(c);
    if (store.chats.length > MAX_CHATS) {   // vượt giới hạn: bỏ trang cũ nhất
        const old = store.chats.filter(x => x.id !== c.id).sort((a, b) => a.updatedAt - b.updatedAt)[0];
        store.chats = store.chats.filter(x => x !== old);
    }
    openChat(c);
}

function deleteChat(id) {
    if (busy) return;
    const c = store.chats.find(x => x.id === id);
    if (!c) return;
    if (hasContent(c.messages) && !confirm('Xoá cuộc trò chuyện này?')) return;
    store.chats = store.chats.filter(x => x.id !== id);
    if (id === store.activeId) {
        const next = [...store.chats].sort((a, b) => b.updatedAt - a.updatedAt)[0] || (() => { const n = makeChat(); store.chats.push(n); return n; })();
        openChat(next);
    } else {
        saveStore();
        renderChatList();
    }
}

/* ---------- Hiển thị tin nhắn ---------- */
// Loại nhóm: system | me | them
const groupType = m => m.sender === 'system' ? 'system' : m.sender === 'me' ? 'me' : 'them';

// Tin nhắn ở index có phải là tin đầu của một nhóm không (đổi loại)
function isGroupStart(index) {
    return index <= 0 || groupType(messages[index]) !== groupType(messages[index - 1]);
}

// Lùi về tin đầu nhóm gần nhất (để tải/vẽ luôn cả nhóm, không bao giờ cắt giữa chừng)
function snapToGroupStart(index) {
    while (index > 0 && !isGroupStart(index)) index--;
    return index;
}

function makeMessageRow(message) {
    if (message.sender === 'system') {
        return el('div', 'system-note', message.text);
    }

    const isMe = message.sender === 'me';
    const editing = isMe && editingMessageId === String(message.id);
    const row = el('article', `row ${isMe ? 'me' : 'them'}${editing ? ' editing' : ''}`);
    if (editing) {
        row.dataset.messageId = message.id;
        const editor = el('div', 'bubble inline-edit-bubble');
        const input = el('textarea', 'inline-edit-input');
        input.value = message.text;
        input.rows = 1;
        input.setAttribute('aria-label', 'Sửa tin nhắn');
        editor.appendChild(input);
        const actions = el('div', 'inline-edit-actions');
        const cancel = el('button', 'inline-edit-button inline-edit-cancel', 'Cancel');
        cancel.type = 'button';
        const submit = el('button', 'inline-edit-button inline-edit-send', 'Send');
        submit.type = 'button';
        actions.append(cancel, submit);
        editor.appendChild(actions);
        row.appendChild(editor);
        return row;
    }
    const body = el('div', 'bubble', message.text);
    row.appendChild(body);
    if (isMe) {
        body.appendChild(el('span', 'tail'));
        const actions = el('div', 'message-meta user-actions');
        actions.dataset.messageId = message.id;
        actions.appendChild(makeTimeLabel(message));
        actions.appendChild(makeCopyButton(message));
        const edit = makeActionButton('edit-button', 'Sửa tin nhắn', 'M3 21l3.75-.75L19.81 7.19a2.12 2.12 0 0 0-3-3L3.75 17.25 3 21ZM15.5 5.5l3 3');
        edit.dataset.messageId = message.id;
        edit.disabled = busy || loading;
        actions.appendChild(edit);
        row.appendChild(actions);
    }
    if (message.sender === 'other') {
        const actions = el('div', 'feedback-actions');
        actions.dataset.messageId = message.id;
        const voted = Number(message.feedback) || 0;
        actions.appendChild(makeFeedbackButton(1, voted === 1, canVoteForMessage(message, 'like')));
        actions.appendChild(makeFeedbackButton(-1, voted === -1, canVoteForMessage(message, 'dislike')));
        const regenerate = makeActionButton('regenerate-button', 'Buộc suy luận lại', 'M20 11a8.1 8.1 0 0 0-15.5-2M4 4v5h5m-5 4a8.1 8.1 0 0 0 15.5 2M20 20v-5h-5');
        regenerate.disabled = !modelReady || !message.buttons?.regenerate?.isEnable;
        regenerate.dataset.messageId = message.id;
        actions.appendChild(regenerate);
        actions.appendChild(makeCopyButton(message));
        const more = makeActionButton('more-button', 'Thêm tùy chọn', 'M5 12h.01M12 12h.01M19 12h.01', true);
        more.disabled = !message.buttons?.more?.isEnable;
        actions.appendChild(more);
        actions.appendChild(makeTimeLabel(message));
        row.appendChild(actions);
    } else if (message.meta) {
        row.appendChild(el('div', 'meta', message.meta));
    }
    return row;
}

function makeTimeLabel(message) {
    const time = el('span', 'message-time', fmtTime(message.createdAt));
    time.dataset.ts = message.createdAt;
    return time;
}

function beginEditMessage(message) {
    if (busy || loading || message?.sender !== 'me') return;
    editingMessageId = String(message.id);
    syncControls();
    renderInitialHistory();
    requestAnimationFrame(() => {
        const input = list.querySelector('.inline-edit-input');
        if (!input) return;
        input.style.height = 'auto';
        input.focus();
    });
}

function cancelEditMessage(render = true) {
    editingMessageId = null;
    syncControls();
    if (render) renderInitialHistory();
}

function canVoteForMessage(message, buttonName) {
    return modelReady && !!message.question && message.buttons?.[buttonName]?.isEnable &&
        (!!M?._wasm_bit_llm_feedback_text || (!!message.feedbackReady && !!M?._wasm_bit_llm_feedback));
}

function refreshFeedbackButtons() {
    list.querySelectorAll('.feedback-actions').forEach(actions => {
        const message = messages.find(item => String(item.id) === actions.dataset.messageId);
        if (!message) return;
        const voted = Number(message.feedback) || 0;
        actions.querySelectorAll('.feedback-button').forEach(button => {
            const key = Number(button.dataset.feedback) > 0 ? 'like' : 'dislike';
            const selected = Number(button.dataset.feedback) === voted;
            button.disabled = !canVoteForMessage(message, key);
            button.classList.toggle('is-selected', selected);
            button.setAttribute('aria-pressed', selected ? 'true' : 'false');
        });
        const regenerate = actions.querySelector('.regenerate-button');
        if (regenerate) regenerate.disabled = !modelReady || !message.buttons?.regenerate?.isEnable;
        const copy = actions.querySelector('.copy-button');
        if (copy) copy.disabled = !message.buttons?.copy?.isEnable;
        const more = actions.querySelector('.more-button');
        if (more) more.disabled = !message.buttons?.more?.isEnable;
    });
}

function makeActionButton(className, label, pathData, round = false) {
    const button = el('button', `toolbar-button ${className}`);
    button.type = 'button';
    button.title = label;
    button.setAttribute('aria-label', label);
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', pathData);
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('stroke-linejoin', 'round');
    if (round) path.setAttribute('stroke-width', '3');
    svg.appendChild(path);
    button.appendChild(svg);
    return button;
}

function makeFeedbackButton(value, selected, enabled) {
    const label = value > 0 ? 'Câu trả lời hữu ích' : 'Câu trả lời chưa hữu ích';
    const button = el('button', `feedback-button${selected ? ' is-selected' : ''}`);
    button.type = 'button';
    button.dataset.feedback = value;
    button.disabled = !enabled;
    button.setAttribute('aria-label', label);
    button.setAttribute('aria-pressed', selected ? 'true' : 'false');
    button.title = label;

    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', value > 0
        ? 'M7 10v12H4a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2h3Zm0 0 5-8 1.2.4a3 3 0 0 1 1.9 3.4L14 10h5.2a2.8 2.8 0 0 1 2.7 3.5l-1.8 7A2 2 0 0 1 18.2 22H7'
        : 'M7 14V2H4a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h3Zm0 0 5 8 1.2-.4a3 3 0 0 0 1.9-3.4L14 14h5.2a2.8 2.8 0 0 0 2.7-3.5l-1.8-7A2 2 0 0 0 18.2 2H7');
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('stroke-linejoin', 'round');
    svg.appendChild(path);
    button.appendChild(svg);
    return button;
}

function makeCopyButton(message) {
    const button = el('button', 'copy-button');
    button.type = 'button';
    button.dataset.messageId = message.id;
    button.disabled = message.buttons?.copy?.isEnable === false;
    button.title = 'Sao chép tin nhắn';
    button.setAttribute('aria-label', 'Sao chép tin nhắn');
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', 'M8 8V5a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-3M6 8h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2Z');
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('stroke-linejoin', 'round');
    svg.appendChild(path);
    button.prepend(svg);
    return button;
}

list.addEventListener('click', async event => {
    const inlineCancel = event.target.closest('.inline-edit-cancel');
    if (inlineCancel) {
        cancelEditMessage();
        return;
    }
    const inlineSubmit = event.target.closest('.inline-edit-send');
    if (inlineSubmit) {
        const row = inlineSubmit.closest('.row.me');
        const message = messages.find(item => String(item.id) === row?.dataset.messageId);
        const input = row?.querySelector('.inline-edit-input');
        if (!message || !input?.value.trim()) return;
        $('q').value = input.value.trim();
        send();
        return;
    }
    const editButton = event.target.closest('.edit-button');
    if (editButton) {
        const message = messages.find(item => String(item.id) === editButton.dataset.messageId);
        if (message) beginEditMessage(message);
        return;
    }
    const regenerateButton = event.target.closest('.regenerate-button');
    if (regenerateButton) {
        const message = messages.find(item => String(item.id) === regenerateButton.dataset.messageId);
        if (message) regenerateAnswer(message);
        return;
    }
    const copyButton = event.target.closest('.copy-button');
    if (copyButton) {
        const message = messages.find(item => String(item.id) === copyButton.dataset.messageId);
        if (!message || !message.buttons?.copy?.isEnable) return;
        try {
            if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(message.text);
            else {
                const input = document.createElement('textarea');
                input.value = message.text;
                input.style.position = 'fixed';
                input.style.opacity = '0';
                document.body.appendChild(input);
                input.select();
                const copied = document.execCommand('copy');
                input.remove();
                if (!copied) throw new Error('clipboard unavailable');
            }
            copyButton.blur();
            const path = copyButton.querySelector('svg path');
            if (path) {
                const copyPath = 'M8 8V5a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-3M6 8h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2Z';
                path.setAttribute('d', 'm5 12 4 4L19 6');
                copyButton.style.color = '#fff';
                clearTimeout(copyFeedbackTimers.get(copyButton));
                copyFeedbackTimers.set(copyButton, setTimeout(() => {
                    if (!copyButton.isConnected) return;
                    path.setAttribute('d', copyPath);
                    copyButton.style.removeProperty('color');
                    copyFeedbackTimers.delete(copyButton);
                }, 1000));
            }
        } catch {
            copyButton.title = 'Không thể sao chép';
        }
        return;
    }
    const button = event.target.closest('.feedback-button');
    if (!button || button.disabled || (!M?._wasm_bit_llm_feedback_text && !M?._wasm_bit_llm_feedback)) return;
    const actions = button.closest('.feedback-actions');
    const message = messages.find(item => String(item.id) === actions?.dataset.messageId);
    if (!message || !message.question) return;
    const vote = Number(button.dataset.feedback);
    const previousVote = Number(message.feedback) || 0;
    const nextVote = previousVote === vote ? 0 : vote;
    if (M._wasm_bit_llm_feedback_text) {
        const qBytes = enc.encode(message.question), aBytes = enc.encode(message.text);
        const qLen = Math.min(qBytes.length, 4095), aLen = Math.min(aBytes.length, 4095);
        heap().set(qBytes.subarray(0, qLen), M._wasm_bit_llm_get_input_ptr());
        heap().set(aBytes.subarray(0, aLen), M._wasm_bit_llm_get_output_ptr());
        // Apply the opposite signal to withdraw an earlier vote, then apply
        // the new one if the user switched from like to dislike or vice versa.
        if (previousVote) M._wasm_bit_llm_feedback_text(qLen, aLen, -previousVote);
        if (nextVote) M._wasm_bit_llm_feedback_text(qLen, aLen, nextVote);
    } else {
        if (!message.feedbackReady) return;
        if (nextVote) M._wasm_bit_llm_feedback(nextVote);
    }
    message.feedback = nextVote;
    message.feedbackReady = false;
    message.buttons.like.isEnable = true;
    message.buttons.dislike.isEnable = true;
    saveStore();

    actions.querySelectorAll('.feedback-button').forEach(action => {
        const selected = Number(action.dataset.feedback) === message.feedback;
        action.classList.toggle('is-selected', selected);
        action.setAttribute('aria-pressed', selected ? 'true' : 'false');
        action.disabled = false;
    });
});

list.addEventListener('input', event => {
    const input = event.target.closest('.inline-edit-input');
    if (!input) return;
    input.style.height = 'auto';
});

list.addEventListener('keydown', event => {
    if (!event.target.closest('.inline-edit-input')) return;
    if (event.key === 'Escape') {
        event.preventDefault();
        cancelEditMessage();
    } else if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        list.querySelector('.inline-edit-send')?.click();
    }
});

async function regenerateAnswer(message) {
    const index = messages.indexOf(message);
    const question = typeof message?.question === 'string' ? message.question : '';
    const previousAnswer = message?.text || '';
    if (!modelReady || busy || index < 0 || !question || !message.buttons?.regenerate?.isEnable) return;
    message.buttons.regenerate.isEnable = false;
    busy = true;
    syncControls();
    messages.splice(index);
    activeChat().updatedAt = Date.now();
    saveStore();
    renderInitialHistory();
    showTyping();
    try {
        const result = await runFreshInference(question, previousAnswer);
        addMessage('other', result.out || 'Mô hình chưa tạo được câu trả lời.', {
            feedbackReady: result.feedbackReady, question, force: true
        });
    } catch (err) {
        addMessage('system', 'Lỗi tạo lại câu trả lời: ' + (err?.message || String(err)), { force: true });
    } finally {
        hideTyping();
        busy = false;
        syncControls();
    }
}

// Tạo một nhóm rỗng bắt đầu tại index.
function makeGroup(index) {
    const type = groupType(messages[index]);
    const group = el('div', `group group-${type}`);
    group.dataset.start = index;
    group.dataset.end = index;
    return [group];
}

// Vẽ các tin [from, to) thành các nhóm hoàn chỉnh. `from` phải là đầu nhóm.
function buildGroups(from, to) {
    const nodes = [];
    let current = null;
    for (let i = from; i < to; i++) {
        if (i === from || isGroupStart(i)) {
            const made = makeGroup(i);
            nodes.push(...made);
            current = made[made.length - 1];
        }
        current.appendChild(makeMessageRow(messages[i]));
        current.dataset.end = i;
    }
    return nodes;
}

function isNearBottom() {
    return list.scrollHeight - list.scrollTop - list.clientHeight < 90;
}

function scrollBottom() {
    requestAnimationFrame(() => {
        list.scrollTo({
            top: list.scrollHeight,
            behavior: 'smooth'
        });
    });
}

function showIntro() {
    const intro = el('section', 'intro');
    intro.appendChild(el('div', 'logo', 'B'));
    intro.appendChild(el('h2', null, 'NNUE_Cut'));
    intro.appendChild(el('p', null, 'Nạp tệp .bin ở thanh bên trái rồi nhắn tin. Mô hình trả lời từng câu hỏi độc lập, ngay trên trình duyệt của bạn.'));
    list.appendChild(intro);
}

function renderInitialHistory() {
    list.replaceChildren();
    typingEl = null;
    if (messages.length === 0) {
        renderedStart = 0;
        showIntro();
        return;
    }

    renderedStart = snapToGroupStart(Math.max(0, messages.length - INITIAL_RENDER_COUNT));
    const fragment = document.createDocumentFragment();
    for (const node of buildGroups(renderedStart, messages.length)) fragment.appendChild(node);
    list.appendChild(fragment);
    if (busy) showTyping();
    scrollBottom();
}

function loadOlderMessages() {
    if (loadingOlder || renderedStart <= 0) return;
    loadingOlder = true;

    const oldHeight = list.scrollHeight;
    const oldTop = list.scrollTop;
    // Luôn tải trọn nhóm: lùi nextStart về đầu nhóm. renderedStart vốn đã là đầu nhóm nên không cắt nhóm nào.
    const nextStart = snapToGroupStart(Math.max(0, renderedStart - OLDER_BATCH_SIZE));
    const fragment = document.createDocumentFragment();
    for (const node of buildGroups(nextStart, renderedStart)) fragment.appendChild(node);
    list.insertBefore(fragment, list.firstChild);
    renderedStart = nextStart;

    list.scrollTop = oldTop + (list.scrollHeight - oldHeight);
    loadingOlder = false;
}

function showTyping() {
    if (typingEl) return;
    typingEl = el('article', 'row them typing');
    const body = el('div', 'bubble');
    for (let i = 0; i < 3; i++) body.appendChild(el('span', 'dot'));
    typingEl.appendChild(body);
    list.appendChild(typingEl);
}

function hideTyping() {
    if (typingEl) typingEl.remove();
    typingEl = null;
}

function addMessage(sender, text, { meta = '', feedbackReady = false, question = '', force = false } = {}) {
    const stickToBottom = force || isNearBottom();
    hideTyping();

    // Lần đầu có tin nhắn: bỏ màn hình giới thiệu
    const introEl = list.querySelector('.intro');
    if (introEl) introEl.remove();

    const message = { id: Date.now() + Math.random(), sender, text, createdAt: Date.now() };
    if (meta) message.meta = meta;
    if (sender === 'other') {
        message.feedbackReady = feedbackReady;
        message.question = question;
        message.buttons = {
            like: { isEnable: true }, dislike: { isEnable: true },
            regenerate: { isEnable: true }, copy: { isEnable: true }, more: { isEnable: true }
        };
    } else if (sender === 'me') {
        message.buttons = { copy: { isEnable: true } };
    }
    messages.push(message);

    const chat = activeChat();
    chat.updatedAt = message.createdAt;
    if (!chat.title && sender === 'me') chat.title = deriveTitle([message]);
    saveStore();
    renderChatList();

    // Append-only: không xoá hoặc render lại các tin nhắn đang có trong DOM.
    const index = messages.length - 1;
    if (index >= renderedStart) {
        const lastGroup = list.lastElementChild;
        if (!isGroupStart(index) && lastGroup?.classList.contains('group') && +lastGroup.dataset.end === index - 1) {
            // Cùng nhóm với tin trước: chỉ nối thêm vào nhóm hiện có
            lastGroup.appendChild(makeMessageRow(message));
            lastGroup.dataset.end = index;
        } else {
            const fragment = document.createDocumentFragment();
            for (const node of buildGroups(index, index + 1)) fragment.appendChild(node);
            list.appendChild(fragment);
        }
    }

    if (busy && sender !== 'other' && sender !== 'system') showTyping();
    if (stickToBottom) scrollBottom();
}

list.addEventListener('scroll', () => {
    if (list.scrollTop <= 48) loadOlderMessages();
}, { passive: true });

function autosize() {
    const q = $('q');
    q.style.height = 'auto';
    q.style.height = Math.min(q.scrollHeight, 120) + 'px';
}

async function send() {
    const text = $('q').value.trim();
    if (!text || busy || loading || (!M && !editingMessageId)) return;

    const editId = editingMessageId;
    let editedIndex = -1;
    let editedMessage = null;
    if (editId) {
        editedIndex = messages.findIndex(item => String(item.id) === editId && item.sender === 'me');
        editedMessage = messages[editedIndex];
        if (!editedMessage) return;
    }

    $('q').value = '';
    autosize();
    if (editId) {
        editedMessage.text = text;
        editedMessage.createdAt = Date.now();
        messages.splice(editedIndex + 1);
        editingMessageId = null;
        cancelEditMessage(false);
        const chat = activeChat();
        chat.updatedAt = editedMessage.createdAt;
        chat.title = deriveTitle(messages);
        saveStore();
        renderInitialHistory();
    } else {
        addMessage('me', text, { force: true });
    }

    if (!modelReady) {
        addMessage('system', 'Chưa nạp mô hình — hãy chọn tệp .bin ở thanh bên trái.');
        return;
    }

    busy = true;
    syncControls();
    showTyping();
    scrollBottom();

    try {
        await new Promise(resolve => setTimeout(resolve, 20));
        const result = await runInference(text);

        hideTyping();
        addMessage('other', result.out || '(mô hình chưa phản hồi được)', {
            feedbackReady: result.feedbackReady, question: text,
            force: true
        });
    } catch (err) {
        hideTyping();
        addMessage('system', 'Lỗi suy luận: ' + (err?.message || String(err)), { force: true });
    } finally {
        busy = false;
        syncControls();
        $('ans').textContent = '';
        $('q').focus();
    }
}

$('ask').addEventListener('click', send);
$('q').addEventListener('input', autosize);
$('q').addEventListener('keydown', event => {
    if (event.key === 'Escape' && editingMessageId) {
        event.preventDefault();
        cancelEditMessage();
        return;
    }
    if (event.key !== 'Enter' || event.shiftKey || event.isComposing || event.keyCode === 229) return;
    event.preventDefault();
    send();
});
$('newChat').addEventListener('click', createChat);   // tạo trang mới, KHÔNG xoá trang cũ
$('menuBtn').addEventListener('click', () => document.body.classList.toggle('side-open'));
$('scrim').addEventListener('click', () => document.body.classList.remove('side-open'));

loadStore();
renderChatList();
renderInitialHistory();
