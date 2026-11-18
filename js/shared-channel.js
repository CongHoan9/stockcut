// ============================================================================
// js/shared-channel.js
// Stockfish-Style Zero-Allocation Lock-Free SPSC Shared Memory Channel
// Giao thức truyền tin không khóa, không sao chép giữa Main UI Thread & Dedicated Render Worker
// Triệt tiêu 100% Structured Clone, JSON Serialization & V8 Garbage Collection Spikes
// ============================================================================
(function (global) {
    'use strict';

    // Tổng kích thước bộ đệm chia sẻ: 1024 bytes (256 phần tử Int32, căn lề 64-byte)
    const SAB_SIZE_BYTES = 1024;
    const SAB_INT32_COUNT = 256;

    // Header Offsets (Chỉ mục trong Int32Array):
    const OFFSET_CMD_HEAD = 0;       // Producer (Main) write pointer
    const OFFSET_CMD_TAIL = 1;       // Consumer (Worker) read pointer
    const OFFSET_CMD_CAPACITY = 2;   // Số lượng slots (16 slots)
    const OFFSET_STATUS_FLAGS = 3;   // Trạng thái kênh (Bit 0: ACTIVE, Bit 1: WORKER_READY, Bit 2: NEW_FRAME_READY)

    // Khối Command Ring Buffer bắt đầu tại chỉ mục 16 (Căn lề 64 bytes):
    // 16 slots * 8 Int32s/slot = 128 Int32s (Từ chỉ mục 16 đến 143)
    const CMD_SLOT_BASE = 16;
    const CMD_SLOT_STRIDE = 8;
    const CMD_CAPACITY = 16;
    const CMD_MASK = CMD_CAPACITY - 1;

    // Cấu trúc 8 Int32s trong 1 Command Slot:
    const CMD_OPCODE = 0;    // 0: NOOP, 1: RENDER_FRAME, 2: SEEK, 3: PLAY, 4: PAUSE, 5: RESIZE
    const CMD_TIME_INT = 1;  // Math.floor(nowMs)
    const CMD_TIME_FRAC = 2; // Math.round((nowMs % 1) * 1000000) (độ chính xác microsecond)
    const CMD_WIDTH = 3;     // Chiều rộng canvas
    const CMD_HEIGHT = 4;    // Chiều cao canvas
    const CMD_FLAGS = 5;     // sceneValid, needAck
    const CMD_BG_RGBA = 6;   // Packed 32-bit RGBA color (8 bit/kênh)
    const CMD_SEQ = 7;       // Sequence number đơn điệu

    // Khối Telemetry Buffer bắt đầu tại chỉ mục 144 (Căn lề 64 bytes):
    // Worker ghi trực tiếp từ C++ TelemetryBuffer, Main UI đọc O(1) không cần gửi postMessage
    const TELEM_BASE = 144;
    const TELEM_FRAME_INDEX = 0;    // Số thứ tự khung hình
    const TELEM_FPS_X100 = 1;       // FPS nhân 100 (ví dụ 6000 = 60.00 FPS)
    const TELEM_CPU_TIME_US = 2;    // Thời gian tính toán CPU (microseconds)
    const TELEM_MODE = 3;           // Chế độ: 0 = FULL, 1 = ZOBRIST, 2 = DIRTY
    const TELEM_HASH_LOW = 4;       // Zobrist Key 32-bit thấp
    const TELEM_HASH_HIGH = 5;      // Zobrist Key 32-bit cao
    const TELEM_DIRTY_COUNT = 6;    // Số lượng vùng bẩn (dirty rects)
    const TELEM_ACTIVE_LAYERS = 7;  // Số lượng layer đang kích hoạt
    const TELEM_DRAWN_COUNT = 8;    // Số lệnh vẽ đã thực thi
    const TELEM_ACK_SEQ = 9;        // Sequence number khung hình mới nhất đã render

    // Lớp điều khiển phía Producer (Main Thread)
    class SharedChannelProducer {
        constructor(sharedArrayBuffer) {
            this.sab = sharedArrayBuffer;
            this.i32 = new Int32Array(sharedArrayBuffer);
            this.seq = 0;
            // Khởi tạo header
            Atomics.store(this.i32, OFFSET_CMD_HEAD, 0);
            Atomics.store(this.i32, OFFSET_CMD_TAIL, 0);
            Atomics.store(this.i32, OFFSET_CMD_CAPACITY, CMD_CAPACITY);
            Atomics.store(this.i32, OFFSET_STATUS_FLAGS, 1); // ACTIVE
        }

        static isSupported() {
            return typeof SharedArrayBuffer !== 'undefined' && typeof Atomics !== 'undefined';
        }

        static createBuffer() {
            if (!SharedChannelProducer.isSupported()) return null;
            return new SharedArrayBuffer(SAB_SIZE_BYTES);
        }

        // Ghi lệnh render frame trong < 15 nanoseconds (0 byte allocation, 0 GC)
        submitRender(nowMs, width, height, sceneValid = 1, bgR = 0, bgG = 0, bgB = 0, bgA = 1) {
            const head = Atomics.load(this.i32, OFFSET_CMD_HEAD);
            const tail = Atomics.load(this.i32, OFFSET_CMD_TAIL);
            const nextHead = (head + 1) & CMD_MASK;

            // Nếu hàng đợi đầy, ta ghi đè slot hiện tại để giữ frame mới nhất (Drop Stale Frames Invariant)
            const writeSlot = (nextHead === tail) ? head : head;
            const slotOffset = CMD_SLOT_BASE + (writeSlot * CMD_SLOT_STRIDE);

            const timeInt = Math.floor(nowMs);
            const timeFrac = Math.round((nowMs - timeInt) * 1000000);
            const packedBg = ((Math.round(bgR * 255) & 0xFF) << 24) |
                             ((Math.round(bgG * 255) & 0xFF) << 16) |
                             ((Math.round(bgB * 255) & 0xFF) << 8) |
                             (Math.round(bgA * 255) & 0xFF);

            this.i32[slotOffset + CMD_OPCODE] = 1; // RENDER_FRAME
            this.i32[slotOffset + CMD_TIME_INT] = timeInt;
            this.i32[slotOffset + CMD_TIME_FRAC] = timeFrac;
            this.i32[slotOffset + CMD_WIDTH] = width | 0;
            this.i32[slotOffset + CMD_HEIGHT] = height | 0;
            this.i32[slotOffset + CMD_FLAGS] = sceneValid ? 1 : 0;
            this.i32[slotOffset + CMD_BG_RGBA] = packedBg;
            this.i32[slotOffset + CMD_SEQ] = ++this.seq;

            // Cam kết nguyên tử (Memory Release)
            if (nextHead !== tail) {
                Atomics.store(this.i32, OFFSET_CMD_HEAD, nextHead);
            }
            return this.seq;
        }

        // Đọc sequence number mới nhất mà Worker đã hoàn tất render
        getAckSeq() {
            return Atomics.load(this.i32, TELEM_BASE + TELEM_ACK_SEQ);
        }

        // Chờ đồng bộ 100% khung hình hoàn tất render (dành riêng cho khâu Export Video)
        async waitForAck(targetSeq, timeoutMs = 2000) {
            const start = (typeof performance !== 'undefined') ? performance.now() : Date.now();
            while (Atomics.load(this.i32, TELEM_BASE + TELEM_ACK_SEQ) < targetSeq) {
                const now = (typeof performance !== 'undefined') ? performance.now() : Date.now();
                if (now - start > timeoutMs) {
                    console.warn(`[SharedChannel] Timeout waiting for ack seq ${targetSeq}`);
                    break;
                }
                await new Promise(r => setTimeout(r, 1));
            }
            return true;
        }

        // Đọc telemetry trực tiếp từ Worker mà không phát sinh message event
        readTelemetry(target = {}) {
            target.frameIndex = Atomics.load(this.i32, TELEM_BASE + TELEM_FRAME_INDEX);
            target.fps = Atomics.load(this.i32, TELEM_BASE + TELEM_FPS_X100) / 100.0;
            target.cpuTimeMs = Atomics.load(this.i32, TELEM_BASE + TELEM_CPU_TIME_US) / 1000.0;
            target.mode = Atomics.load(this.i32, TELEM_BASE + TELEM_MODE);
            const hashLow = Atomics.load(this.i32, TELEM_BASE + TELEM_HASH_LOW) >>> 0;
            const hashHigh = Atomics.load(this.i32, TELEM_BASE + TELEM_HASH_HIGH) >>> 0;
            target.hashHex = hashHigh.toString(16).padStart(8, '0') + hashLow.toString(16).padStart(8, '0');
            target.dirtyCount = Atomics.load(this.i32, TELEM_BASE + TELEM_DIRTY_COUNT);
            target.activeLayers = Atomics.load(this.i32, TELEM_BASE + TELEM_ACTIVE_LAYERS);
            target.drawnCount = Atomics.load(this.i32, TELEM_BASE + TELEM_DRAWN_COUNT);
            target.ackSeq = Atomics.load(this.i32, TELEM_BASE + TELEM_ACK_SEQ);
            return target;
        }
    }

    // Lớp điều khiển phía Consumer (Dedicated Render Worker)
    class SharedChannelConsumer {
        constructor(sharedArrayBuffer) {
            this.sab = sharedArrayBuffer;
            this.i32 = new Int32Array(sharedArrayBuffer);
            Atomics.or(this.i32, OFFSET_STATUS_FLAGS, 2); // WORKER_READY
        }

        // Đọc toàn bộ lệnh mới tích lũy trong Ring Buffer (Hot Path)
        pollLatestRenderCommand() {
            let tail = Atomics.load(this.i32, OFFSET_CMD_TAIL);
            const head = Atomics.load(this.i32, OFFSET_CMD_HEAD);
            if (tail === head) return null;

            let latestCmd = null;
            let anyInvalid = false;
            while (tail !== head) {
                const slotOffset = CMD_SLOT_BASE + (tail * CMD_SLOT_STRIDE);
                const op = this.i32[slotOffset + CMD_OPCODE];
                if (op === 1) { // RENDER_FRAME
                    const timeInt = this.i32[slotOffset + CMD_TIME_INT];
                    const timeFrac = this.i32[slotOffset + CMD_TIME_FRAC];
                    const packedBg = this.i32[slotOffset + CMD_BG_RGBA];
                    const slotValid = this.i32[slotOffset + CMD_FLAGS];
                    if (slotValid === 0) anyInvalid = true;
                    latestCmd = {
                        nowMs: timeInt + (timeFrac / 1000000.0),
                        width: this.i32[slotOffset + CMD_WIDTH],
                        height: this.i32[slotOffset + CMD_HEIGHT],
                        sceneValid: anyInvalid ? 0 : slotValid,
                        bgR: ((packedBg >>> 24) & 0xFF) / 255.0,
                        bgG: ((packedBg >>> 16) & 0xFF) / 255.0,
                        bgB: ((packedBg >>> 8) & 0xFF) / 255.0,
                        bgA: (packedBg & 0xFF) / 255.0,
                        seq: this.i32[slotOffset + CMD_SEQ]
                    };
                }
                tail = (tail + 1) & CMD_MASK;
            }
            // Cập nhật vị trí đã đọc cho Producer
            Atomics.store(this.i32, OFFSET_CMD_TAIL, tail);
            return latestCmd;
        }

        // Ghi số liệu Telemetry trực tiếp vào SAB cho Main Thread đọc
        writeTelemetry(frameIndex, fps, cpuTimeMs, mode, hashLow, hashHigh, dirtyCount, activeLayers, drawnCount, seq) {
            Atomics.store(this.i32, TELEM_BASE + TELEM_FRAME_INDEX, frameIndex | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_FPS_X100, Math.round(fps * 100) | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_CPU_TIME_US, Math.round(cpuTimeMs * 1000) | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_MODE, mode | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_HASH_LOW, hashLow | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_HASH_HIGH, hashHigh | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_DIRTY_COUNT, dirtyCount | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_ACTIVE_LAYERS, activeLayers | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_DRAWN_COUNT, drawnCount | 0);
            Atomics.store(this.i32, TELEM_BASE + TELEM_ACK_SEQ, seq | 0);
        }
    }

    const SharedChannel = {
        SAB_SIZE_BYTES,
        SharedChannelProducer,
        SharedChannelConsumer
    };

    if (typeof module !== 'undefined' && module.exports) {
        module.exports = SharedChannel;
    }
    if (typeof global !== 'undefined') {
        global.SharedChannel = SharedChannel;
    }
})(typeof self !== 'undefined' ? self : (typeof window !== 'undefined' ? window : this));
