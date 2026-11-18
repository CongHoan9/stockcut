# VBO Subsystem Architecture (Instanced Batching)

## 1. Tổng quan (Overview)
Thư mục `vbo/` phụ trách đóng gói và chuyển dữ liệu đỉnh hình học xuống GPU:
- **`VboQuadInstance.h`**: Cấu trúc dữ liệu đại diện cho 1 Quad hình chữ nhật căn lề 16 bytes, tổng kích thước đúng 48 bytes (pos[4], uv[4], color, rotation, pivotX, pivotY).
- **`vbo_batcher.h / .cpp`**: Bộ gom nhóm đỉnh (Quad Batcher). Chuyển đổi hàng loạt các lệnh vẽ thành mảng float liên tục và nạp vào GPU VBO trong đúng 1 lệnh gọi OpenGL ES duy nhất.

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Gọi `glDrawArrays` riêng lẻ cho từng ảnh/chữ gây nghẽn hàng đợi lệnh GPU (Command Queue Bottleneck).
- **Khắc phục triệt để**:
  - Instanced Draw Calls và Dynamic Batching: Gom tối đa hàng trăm quad vào 1 batch pass, sử dụng mảng tĩnh `TOTAL_VBO_FLOATS` không cấp phát heap.
