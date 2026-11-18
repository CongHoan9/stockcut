/* ============================================================================
 * llm-chat.js — Đã sửa lại cấu trúc DOM dùng .group chuẩn theo đúng app.js
 * ============================================================================ */
(() => {
    'use strict';
    const STORE_KEY_V2 = 'bitllm-chats-v2';
    const STORE_KEY_OLD = 'nnue-cut-chat-v1';
    const MAX_STORE = 500;
    const statusEl = document.getElementById('llm-model-status') || document.getElementById('badge');
    const list = document.getElementById('llm-chat-messages') || document.getElementById('message-list');
    const form = document.getElementById('llm-chat-form');
    const input = document.getElementById('llm-chat-input') || document.getElementById('q');
    const sendBtn = document.getElementById('llm-send-btn') || document.getElementById('ask');
    const newChatBtn = document.getElementById('llm-new-chat') || document.getElementById('newChat');
    const enc = new TextEncoder();
    const copyFeedbackTimers = new WeakMap();
    if (!list || !input || !sendBtn) return;
    let worker = null;
    let modelReady = false;
    let busy = false;
    let nextRequestId = 1;
    let pendingRequestId = 0;
    let pendingRegenerateMessage = null;
    let editingMessageId = null;
    let messages = [];
    const el = (tag, cls, text) => {
        const node = document.createElement(tag);
        if (cls) node.className = cls;
        if (text != null) node.textContent = text;
        return node;
    };
    const fmtTime = ts => new Date(ts).toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });
    /* ---------- LƯU TRỮ VÀ NẠP LỊCH SỬ ---------- */
    function sanitizeMessages(arr) {
        if (!Array.isArray(arr)) return [];
        let latestQuestion = '';
        return arr.filter(m => m && typeof m.text === 'string').slice(-MAX_STORE).map(m => {
            const sender = m.sender || (m.role === 'user' ? 'me' : m.role === 'assistant' ? 'other' : 'system');
            if (sender === 'me') latestQuestion = m.text;
            const question = sender === 'other' ? (m.question || latestQuestion) : m.question;
            return {
                id: m.id || (Date.now() + Math.random()),
                sender,
                text: m.text,
                createdAt: m.createdAt || Date.now(),
                question,
                feedback: Number(m.feedback) || 0,
                buttons: m.buttons || {
                    like: { isEnable: true },
                    dislike: { isEnable: true },
                    regenerate: { isEnable: true },
                    copy: { isEnable: true },
                    more: { isEnable: true }
                }
            };
        });
    }
    function loadStore() {
        try {
            const p = JSON.parse(localStorage.getItem(STORE_KEY_V2) || 'null');
            if (p && Array.isArray(p.chats) && p.chats.length > 0) {
                messages = sanitizeMessages(p.chats[0].messages);
                return;
            }
        } catch (_) { }
        try {
            const old = JSON.parse(localStorage.getItem(STORE_KEY_OLD) || '[]');
            if (Array.isArray(old) && old.length > 0) {
                messages = sanitizeMessages(old);
                saveStore();
                return;
            }
        } catch (_) { }
        messages = [];
    }
    function saveStore() {
        try {
            localStorage.setItem(STORE_KEY_V2, JSON.stringify({
                activeId: 'default',
                chats: [{ id: 'default', title: 'Cuộc trò chuyện', createdAt: Date.now(), updatedAt: Date.now(), messages }]
            }));
        } catch (_) { }
    }
    function syncControls() {
        const disabled = busy || !modelReady || !!editingMessageId;
        sendBtn.disabled = disabled || !input.value.trim();
        input.disabled = busy || !!editingMessageId;
        if (newChatBtn) newChatBtn.disabled = busy;
        list.querySelectorAll('.edit-button').forEach(btn => btn.disabled = busy);
    }
    /* ---------- DOM GENERATION (CHUẨN HÓA THEO NHÓM .group) ---------- */
    function groupType(m) {
        return m.sender === 'system' ? 'system' : m.sender === 'me' ? 'me' : 'them';
    }
    function isGroupStart(index) {
        return index <= 0 || groupType(messages[index]) !== groupType(messages[index - 1]);
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
    function makeFeedbackButton(value, selected) {
        const label = value > 0 ? 'Câu trả lời hữu ích' : 'Câu trả lời chưa hữu ích';
        const button = el('button', `feedback-button${selected ? ' is-selected' : ''}`);
        button.type = 'button';
        button.dataset.feedback = value;
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
    function makeTimeLabel(message) {
        const time = el('span', 'message-time', fmtTime(message.createdAt));
        time.dataset.ts = message.createdAt;
        return time;
    }
    function makeMessageRow(message) {
        if (message.sender === 'system') {
            return el('div', 'system-note', message.text);
        }
        const isMe = message.sender === 'me';
        const editing = isMe && editingMessageId === String(message.id);
        const row = el('article', `row ${isMe ? 'me' : 'them'}${editing ? ' editing' : ''}`);
        // Trạng thái Chỉnh sửa tin nhắn (Inline Edit)
        if (editing) {
            row.dataset.messageId = message.id;
            const editor = el('div', 'bubble inline-edit-bubble');
            const editInput = el('textarea', 'inline-edit-input');
            editInput.value = message.text;
            editInput.rows = 1;
            editInput.setAttribute('aria-label', 'Sửa tin nhắn');
            editor.appendChild(editInput);
            const actions = el('div', 'inline-edit-actions');
            const cancelBtn = el('button', 'inline-edit-button inline-edit-cancel', 'Cancel');
            cancelBtn.type = 'button';
            const submitBtn = el('button', 'inline-edit-button inline-edit-send', 'Send');
            submitBtn.type = 'button';
            actions.append(cancelBtn, submitBtn);
            editor.appendChild(actions);
            row.appendChild(editor);
            return row;
        }
        // Cấu trúc Bong bóng tin nhắn chuẩn (Không có Avatar)
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
            edit.disabled = busy;
            actions.appendChild(edit);
            row.appendChild(actions);
        } else {
            // Thanh công cụ phản hồi phía dưới câu trả lời của AI
            const actions = el('div', 'feedback-actions');
            actions.dataset.messageId = message.id;
            const voted = Number(message.feedback) || 0;
            actions.appendChild(makeFeedbackButton(1, voted === 1));
            actions.appendChild(makeFeedbackButton(-1, voted === -1));
            const regenerate = makeActionButton('regenerate-button', 'Buộc suy luận lại', 'M20 11a8.1 8.1 0 0 0-15.5-2M4 4v5h5m-5 4a8.1 8.1 0 0 0 15.5 2M20 20v-5h-5');
            regenerate.disabled = !modelReady || busy;
            regenerate.dataset.messageId = message.id;
            actions.appendChild(regenerate);
            actions.appendChild(makeCopyButton(message));
            const more = makeActionButton('more-button', 'Thêm tùy chọn', 'M5 12h.01M12 12h.01M19 12h.01', true);
            actions.appendChild(more);
            actions.appendChild(makeTimeLabel(message));
            row.appendChild(actions);
        }
        return row;
    }
    function showIntro() {
        const intro = el('section', 'intro');
        intro.appendChild(el('div', 'logo', 'B'));
        intro.appendChild(el('h2', null, 'NNUE_Cut'));
        intro.appendChild(el('p', null, 'Nạp tệp .bin ở thanh bên trái rồi nhắn tin. Mô hình trả lời từng câu hỏi độc lập, ngay trên trình duyệt của bạn.'));
        list.appendChild(intro);
    }
    function renderMessages() {
        list.replaceChildren();
        if (messages.length === 0) {
            showIntro();
            return;
        }
        const frag = document.createDocumentFragment();
        let currentGroup = null;
        for (let i = 0; i < messages.length; i++) {
            const msg = messages[i];
            if (i === 0 || isGroupStart(i)) {
                const type = groupType(msg);
                currentGroup = el('div', `group group-${type}`);
                currentGroup.dataset.start = i;
                frag.appendChild(currentGroup);
            }
            if (currentGroup) {
                currentGroup.appendChild(makeMessageRow(msg));
                currentGroup.dataset.end = i;
            }
        }
        list.appendChild(frag);
        list.scrollTop = list.scrollHeight;
    }
    function showTyping() {
        if (list.querySelector('.row.them.typing')) return;
        const lastGroup = list.lastElementChild;
        const typingGroup = el('div', 'group group-them');
        const typingEl = el('article', 'row them typing');
        const body = el('div', 'bubble');
        for (let i = 0; i < 3; i++) body.appendChild(el('span', 'dot'));
        typingEl.appendChild(body);
        typingGroup.appendChild(typingEl);
        list.appendChild(typingGroup);
        list.scrollTop = list.scrollHeight;
    }
    function hideTyping() {
        list.querySelectorAll('.row.them.typing').forEach(el => el.closest('.group')?.remove() || el.remove());
    }
    function addMessage(sender, text, { question = '' } = {}) {
        hideTyping();
        const intro = list.querySelector('.intro');
        if (intro) intro.remove();
        const message = {
            id: Date.now() + Math.random(),
            sender,
            text: String(text),
            createdAt: Date.now(),
            question,
            feedback: 0
        };
        messages.push(message);
        saveStore();
        const index = messages.length - 1;
        const lastGroup = list.lastElementChild;
        if (!isGroupStart(index) && lastGroup?.classList.contains('group') && lastGroup.classList.contains(`group-${groupType(message)}`)) {
            lastGroup.appendChild(makeMessageRow(message));
            lastGroup.dataset.end = index;
        } else {
            const type = groupType(message);
            const newGroup = el('div', `group group-${type}`);
            newGroup.dataset.start = index;
            newGroup.dataset.end = index;
            newGroup.appendChild(makeMessageRow(message));
            list.appendChild(newGroup);
        }
        list.scrollTop = list.scrollHeight;
        return message;
    }
    /* ---------- XỬ LÝ CHỈNH SỬA & HỦY TIN NHẮN ---------- */
    function beginEditMessage(message) {
        if (busy || message?.sender !== 'me') return;
        editingMessageId = String(message.id);
        syncControls();
        renderMessages();
        requestAnimationFrame(() => {
            const editInput = list.querySelector('.inline-edit-input');
            if (editInput) {
                editInput.focus();
                editInput.style.height = 'auto';
                editInput.style.height = `${editInput.scrollHeight}px`;
            }
        });
    }
    function cancelEditMessage() {
        editingMessageId = null;
        syncControls();
        renderMessages();
    }
    /* ---------- EVENT LISTENERS BẢNG TIN NHẮN ---------- */
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
            const editInput = row?.querySelector('.inline-edit-input');
            if (!message || !editInput?.value.trim()) return;
            input.value = editInput.value.trim();
            cancelEditMessage();
            send();
            return;
        }
        const editButton = event.target.closest('.edit-button');
        if (editButton) {
            const message = messages.find(item => String(item.id) === editButton.dataset.messageId);
            if (message) beginEditMessage(message);
            return;
        }
        const regenerateBtn = event.target.closest('.regenerate-button');
        if (regenerateBtn) {
            const message = messages.find(item => String(item.id) === regenerateBtn.dataset.messageId);
            if (message && message.question && !busy && modelReady) {
                regenerateAnswer(message);
            }
            return;
        }
        const copyButton = event.target.closest('.copy-button');
        if (copyButton) {
            const message = messages.find(item => String(item.id) === copyButton.dataset.messageId);
            if (!message) return;
            try {
                await navigator.clipboard.writeText(message.text);
                const path = copyButton.querySelector('svg path');
                if (path) {
                    const copyPath = 'M8 8V5a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-3M6 8h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2Z';
                    path.setAttribute('d', 'm5 12 4 4L19 6');
                    copyButton.style.color = '#fff';
                    clearTimeout(copyFeedbackTimers.get(copyButton));
                    copyFeedbackTimers.set(copyButton, setTimeout(() => {
                        path.setAttribute('d', copyPath);
                        copyButton.style.removeProperty('color');
                    }, 1000));
                }
            } catch (_) { }
            return;
        }
        const feedbackBtn = event.target.closest('.feedback-button');
        if (feedbackBtn) {
            const actions = feedbackBtn.closest('.feedback-actions');
            const message = messages.find(item => String(item.id) === actions?.dataset.messageId);
            if (!message) return;
            const vote = Number(feedbackBtn.dataset.feedback);
            message.feedback = message.feedback === vote ? 0 : vote;
            saveStore();
            actions.querySelectorAll('.feedback-button').forEach(btn => {
                const selected = Number(btn.dataset.feedback) === message.feedback;
                btn.classList.toggle('is-selected', selected);
                btn.setAttribute('aria-pressed', selected ? 'true' : 'false');
            });
        }
    });
    list.addEventListener('input', event => {
        const editInput = event.target.closest('.inline-edit-input');
        if (editInput) {
            editInput.style.height = 'auto';
            editInput.style.height = `${editInput.scrollHeight}px`;
        }
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
    /* ---------- LUỒNG XỬ LÝ WORKER & SUY LUẬN ---------- */
    function setStatus(state, text, progress) {
        if (!statusEl) return;
        statusEl.className = `badge ${state === 'ready' ? 'ok' : state === 'error' ? 'er' : 'wa'}`;
        statusEl.textContent = state === 'loading' && Number.isFinite(progress) ? `${text} ${progress}%` : text;
    }
    function handleWorkerMessage(event) {
        const data = event.data || {};
        if (data.type === 'status') {
            modelReady = data.state === 'ready';
            setStatus(data.state, data.text || '', data.progress);
            syncControls();
            return;
        }
        if (data.type !== 'result' || data.requestId !== pendingRequestId) return;
        hideTyping();
        if (pendingRegenerateMessage) {
            pendingRegenerateMessage.text = data.error || data.text || '(mô hình chưa phản hồi được)';
            pendingRegenerateMessage.createdAt = Date.now();
            pendingRegenerateMessage = null;
            saveStore();
            renderMessages();
        } else {
            addMessage('other', data.error || data.text || '(mô hình chưa phản hồi được)', {
                question: input.dataset.lastQuestion || ''
            });
        }
        pendingRequestId = 0;
        busy = false;
        syncControls();
        input.focus();
    }
    function startWorker() {
        if (worker) return;
        setStatus('loading', 'Đang khởi tạo LLM WASM…');
        try {
            worker = new Worker(new URL('js/llm-worker.js', document.baseURI).href, { name: 'nnue-cut-llm' });
            worker.addEventListener('message', handleWorkerMessage);
            worker.addEventListener('error', event => setStatus('error', `Lỗi LLM Worker: ${event.message || 'lỗi không xác định'}`));
            worker.postMessage({ type: 'load' });
        } catch (error) {
            setStatus('error', `Không khởi tạo được LLM Worker: ${error.message || error}`);
        }
    }
    async function regenerateAnswer(targetMessage) {
        const index = messages.indexOf(targetMessage);
        if (index < 0 || !targetMessage.question) return;
        busy = true;
        syncControls();
        showTyping();
        pendingRegenerateMessage = targetMessage;
        pendingRequestId = nextRequestId++;
        worker.postMessage({ type: 'infer', requestId: pendingRequestId, text: targetMessage.question });
    }
    function send() {
        const text = input.value.trim();
        if (!text || busy || !worker || !modelReady) return;
        const byteLength = enc.encode(text).length;
        if (byteLength >= 4096) {
            addMessage('system', 'Tin nhắn quá dài (giới hạn 4095 byte).');
            return;
        }
        addMessage('me', text);
        input.value = '';
        input.dataset.lastQuestion = text;
        autosize();
        pendingRequestId = nextRequestId++;
        busy = true;
        syncControls();
        showTyping();
        worker.postMessage({ type: 'infer', requestId: pendingRequestId, text });
    }
    function autosize() {
        input.style.height = 'auto';
        input.style.height = `${Math.min(input.scrollHeight, 120)}px`;
    }
    if (form) {
        form.addEventListener('submit', event => {
            event.preventDefault();
            send();
        });
    }
    sendBtn.addEventListener('click', send);
    input.addEventListener('input', () => {
        autosize();
        syncControls();
    });
    input.addEventListener('keydown', event => {
        if (event.key === 'Enter' && !event.shiftKey && !event.isComposing && event.keyCode !== 229) {
            event.preventDefault();
            send();
        }
    });
    if (newChatBtn) {
        newChatBtn.addEventListener('click', () => {
            if (busy) return;
            messages = [];
            saveStore();
            renderMessages();
            input.focus();
        });
    }
    input.addEventListener('focus', () => {
        startWorker();
    }, { once: true });
    const agentTabBtn = document.querySelector('[data-pane="agent"]');
    if (agentTabBtn) {
        agentTabBtn.addEventListener('click', () => {
            startWorker();
        }, { once: true });
    }
    loadStore();
    renderMessages();
})();