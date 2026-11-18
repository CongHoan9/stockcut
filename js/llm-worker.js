'use strict';
let llmModule = null;
let modelReady = false;
const encoder = new TextEncoder();
const decoder = new TextDecoder();
function postStatus(state, text, progress = null) {
    self.postMessage({ type: 'status', state, text, progress });
}
async function bootAndLoadModel() {
    try {
        importScripts('./llm/bit_llm.js');
        if (typeof createBitLLMModule !== 'function') {
            throw new Error('Không tìm thấy bộ khởi tạo LLM WebAssembly.');
        }
        llmModule = await createBitLLMModule({
            locateFile: file => new URL(`./llm/${file}`, self.location.href).href
        });
        if (!llmModule.HEAPU8) throw new Error('LLM WASM không cung cấp bộ nhớ HEAPU8.');
        llmModule._wasm_bit_llm_init();
        postStatus('loading', 'Đang nạp trọng số LLM…', 0);
        const weightsUrl = new URL('../Asset/llm/bit_llm_weights.bin', self.location.href);
        const response = await fetch(weightsUrl, { cache: 'no-store' });
        if (!response.ok) throw new Error(`Không tải được weights (${response.status}).`);
        const totalHeader = response.headers.get('content-length');
        const contentLength = totalHeader ? Number(totalHeader) : 0;
        const headerSize = llmModule._wasm_bit_llm_get_header_size();
        const chunkCapacity = llmModule._wasm_bit_llm_get_chunk_buffer_size();
        const chunkPtr = llmModule._wasm_bit_llm_get_chunk_buffer_ptr();
        if (!headerSize || !chunkCapacity || !chunkPtr) throw new Error('LLM WASM thiếu buffer nạp model.');
        llmModule._wasm_bit_llm_begin_load();
        let loadedBytes = 0;
        let expectedBytes = headerSize;
        let targetBytes = contentLength;
        let lastProgressAt = 0;
        const pushBytes = data => {
            let cursor = 0;
            while (cursor < data.length) {
                const count = Math.min(chunkCapacity, data.length - cursor, expectedBytes - loadedBytes);
                if (count <= 0) throw new Error('Kích thước weights không hợp lệ.');
                llmModule.HEAPU8.set(data.subarray(cursor, cursor + count), chunkPtr);
                if (!llmModule._wasm_bit_llm_load_chunk_direct(loadedBytes, count)) {
                    throw new Error(loadedBytes < headerSize ? 'Header weights không hợp lệ.' : 'Dữ liệu weights bị lỗi.');
                }
                loadedBytes += count;
                cursor += count;
                if (loadedBytes === headerSize) {
                    expectedBytes = Number(llmModule._wasm_bit_llm_get_stream_bytes());
                    const modelFileBytes = Number(llmModule._wasm_bit_llm_get_file_size());
                    if (!expectedBytes || expectedBytes !== modelFileBytes) throw new Error('Header và kích thước model không khớp.');
                    if (targetBytes && targetBytes !== expectedBytes) throw new Error(`Weights tải về ${targetBytes} byte, model cần ${expectedBytes} byte.`);
                    targetBytes = expectedBytes;
                }
            }
            const now = performance.now();
            if (now - lastProgressAt > 80 || loadedBytes === expectedBytes) {
                lastProgressAt = now;
                const progress = targetBytes ? Math.min(100, Math.round(loadedBytes / targetBytes * 100)) : null;
                postStatus('loading', 'Đang nạp trọng số LLM…', progress);
            }
        };
        if (response.body && response.body.getReader) {
            const reader = response.body.getReader();
            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                if (value?.length) pushBytes(value);
            }
        } else {
            const bytes = new Uint8Array(await response.arrayBuffer());
            for (let offset = 0; offset < bytes.length; offset += chunkCapacity) {
                pushBytes(bytes.subarray(offset, Math.min(bytes.length, offset + chunkCapacity)));
                await new Promise(resolve => setTimeout(resolve, 0));
            }
        }
        if (loadedBytes !== expectedBytes || !llmModule._wasm_bit_llm_finalize_load()) {
            throw new Error('Không thể xác thực trọng số LLM.');
        }
        modelReady = true;
        const vocabCount = llmModule._wasm_bit_llm_get_vocab_count();
        postStatus('ready', `${vocabCount.toLocaleString('vi-VN')} token · LLM WASM sẵn sàng`, 100);
    } catch (error) {
        modelReady = false;
        postStatus('error', error?.message || String(error));
    }
}
self.onmessage = async event => {
    const message = event.data || {};
    if (message.type === 'load') {
        if (!modelReady) await bootAndLoadModel();
        return;
    }
    if (message.type !== 'infer') return;
    if (!modelReady || !llmModule) {
        self.postMessage({ type: 'result', requestId: message.requestId, error: 'Mô hình LLM chưa sẵn sàng.' });
        return;
    }
    try {
        const bytes = encoder.encode(String(message.text || ''));
        if (!bytes.length || bytes.length >= 4096) throw new Error('Tin nhắn quá dài (giới hạn 4095 byte).');
        llmModule.HEAPU8.set(bytes, llmModule._wasm_bit_llm_get_input_ptr());
        const startedAt = performance.now();
        const outputLength = llmModule._wasm_bit_llm_infer(bytes.length);
        const outputPtr = llmModule._wasm_bit_llm_get_output_ptr();
        const answer = decoder.decode(llmModule.HEAPU8.subarray(outputPtr, outputPtr + outputLength)).trim();
        self.postMessage({
            type: 'result',
            requestId: message.requestId,
            text: answer || '(mô hình chưa tạo được câu trả lời)',
            elapsedMs: Math.round(performance.now() - startedAt)
        });
    } catch (error) {
        self.postMessage({ type: 'result', requestId: message.requestId, error: error?.message || String(error) });
    }
};
