# Timeline Subsystem Architecture

## 1. Tổng quan (Overview)
Thư mục `timeline/` chịu trách nhiệm điều phối toàn bộ thời gian, trạng thái project và vị trí frame:
- **`bitboard.h / .cpp`**: Thao tác Bitboard 64-bit để quản lý trạng thái hiển thị, active item và ánh xạ layer.
- **`effect.h / .cpp`**: Đại số tuyến tính hợp nhất SIMD128 (`MathOp` 64B & `EffectTrack` 64B) dung hợp chuỗi $f(x) = m \cdot x + b$ và thực thi `render_layer_branchless` không nhánh rẽ.
- **`gpu_transfer_queue.h / .cpp`**: Hàng đợi chuyển dữ liệu GPU không khóa cho các trang VBO bẩn.
- **`timeline_position.h`**: Quản lý ngăn xếp lịch sử StateInfo và vị trí con trỏ timeline theo mô hình Stockfish.
- **`position/`**: Bộ sinh DisplayList vi sai và các hàm export WASM phục vụ định vị timeline.
- **`project/`**: Trình biên dịch JSON tốc độ cao (0 malloc), tính toán thời lượng và nạp project nguyên khối.
- **`runtime/`**: Runtime điều phối render pass plan, biên dịch đồ thị hiệu ứng (Render Pass Plan) sang mảng phẳng tĩnh cho NativeRenderer.

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Duyệt cây phân cấp đệ quy và truy vấn kiểu item trong Hot Path gây cache miss và branch misprediction.
- **Khắc phục triệt để**:
  - Chuyên biệt hóa kiểu dữ liệu từ đầu, lưu trữ theo mảng phẳng tuyến tính.
  - Phân tích và biên dịch JSON một lần duy nhất ở Cold Path; Hot Path chỉ xử lý các chỉ số bộ nhớ thô O(1).
  - **Tách biệt Authoring State vs. Runtime Evaluated State**: `EvaluatedSlotState` (64B) lưu trữ tọa độ đã tính toán tại mili-giây $T$, 4 KB vừa trọn L1 Data Cache.
  - **Temporal Bitboard Chunking ($O(1)$)**: Phân chia timeline thành các chunk 256ms 64-bit đơn vị. Trả về active bitboard trong 1 chu kỳ máy không duyệt mảng.
  - **Stockfish Differential Accumulator**: Chỉ tính toán lại các slot bẩn (`dirtyBB & activeBB`), slot tĩnh tốn 0 chu kỳ CPU khi phát lại.
  - Hiệu năng benchmark: **13,109,596 FPS** (timeline step) và **1,959,420 FPS** (pipeline 64 image layers, frame time 0.510 microsecond).
