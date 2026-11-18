# Core Subsystem Architecture

## 1. Tổng quan (Overview)
Thư mục `core/` chứa các khối nền tảng thiết yếu của C++ Core Engine:
- **`types.h` / `render_constants.h`**: Định nghĩa kiểu dữ liệu cơ bản (`u8`, `u16`, `u32`, `u64`, `f32`, `f64`), các giới hạn tài nguyên và capacity mảng tĩnh.
- **`math_utils.h / .cpp`**: Thư viện toán học tốc độ cao, xấp xỉ lượng giác, phép nội suy không rẽ nhánh, clamp/lerp.
- **`obs_matrix.h / .cpp`**: Ma trận biến đổi Affine 3x3 tối ưu hóa cho đồ họa 2D phẳng.
- **`geometry_engine.h / .cpp`**: Tính toán bao hình Bounding Box, giao cắt AABB, biến đổi ma trận đỉnh.
- **`render_scale.h / .cpp`**: Quản lý độ co giãn DPR (Device Pixel Ratio) và thích ứng độ phân giải viewport.
- **`pipeline.h / .cpp`**: Điều phối các giai đoạn xử lý khung hình (Decode -> Layout -> Interpolate -> Render).
- **`filter_pipeline.h / .cpp`**: Phân tích chuỗi bộ lọc CSS filter và ánh xạ xuống phần cứng.
- **`display_list.h / .cpp`**: Danh sách lệnh vẽ tuyến tính phẳng (Flat Linear Command Buffer).
- **`style_engine.h / .cpp`**: Động cơ tính toán kiểu dáng CSS và giải quyết thứ tự kế thừa (Cascading Layout).
- **`slot_tracker.h / .cpp`**: Quản lý vòng đời và chỉ mục slot của các item trên timeline.
- **`timeline_keyframe.h / .cpp`**: Động cơ nội suy keyframe từng đoạn (Cubic Bezier, Ease-In-Out, Linear, Hold).
- **`shared_state.h / .cpp`**: Đồng bộ trạng thái đa luồng không khóa (Relaxed Atomic & Lock-free Transfer Policy).
- **`telemetry.h / .cpp`**: Bộ đệm đo đạc hiệu năng tĩnh căn lề 64 bytes (256 bytes struct) tách biệt View và Render throughput.
- **`engine_manager.h / .cpp`**: Bộ điều phối trung tâm giữa các module C++ và bộ nhớ đệm chia sẻ với WebAssembly.
- **`core_engine.cpp` / `wasm_entry.cpp`**: Cổng giao tiếp WebAssembly và điểm xuất hàm extern "C".

## 2. Kiến trúc & Điểm nghẽn triệt tiêu
- **Zero-Allocation**: Không cấp phát heap động (`new`, `malloc`) trong vòng lặp 60 FPS.
- **Cacheline Alignment**: Mọi struct quan trọng đều căn lề 64 bytes (`alignas(64)`), loại bỏ False Sharing.
- **Branchless Math**: Sử dụng đại số tuyến tính SIMD128, triệt tiêu phân nhánh điều kiện trên Hot Path.
