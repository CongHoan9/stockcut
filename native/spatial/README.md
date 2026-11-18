# Spatial Subsystem Architecture (Bitboard & Dirty Scissoring)

## 1. Tổng quan (Overview)
Thư mục `spatial/` quản lý phân vùng không gian 2D và tối ưu hóa diện tích vẽ bẩn:
- **`dirty_rect.h / .cpp`**: Quản lý tập hợp hình chữ nhật bẩn (Dirty Rectangles). Sử dụng thuật toán hợp nhất (coalescing) và kiểm tra bao đóng AABB bằng SIMD128.
- **`spatial_bitboard.h / .cpp`**: Lưới phân vùng không gian bitboard 64-bit (8x8 cells). Ánh xạ vị trí của các layer vào các bit để phát hiện va chạm và giao cắt trong O(1).
- **`natron_roi.h / .cpp`**: Cơ chế Region of Interest (RoI) học tập từ Natron NLE. Xác định chính xác vùng viewport cần tính toán cho từng hiệu ứng, cắt tỉa (scissor) các pixel ngoài màn hình.

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Vẽ lại toàn bộ màn hình 1920x1080 (2.07 triệu pixel) khi chỉ có 1 dòng lyric nhỏ chuyển màu.
- **Khắc phục triệt để**:
  - Hạn chế tối đa số pixel vẽ lại bằng cơ chế `glScissor` và Dirty Incremental Blit.
  - Phân vùng không gian bằng Bitboard giúp loại bỏ toàn bộ các phép duyệt lồng nhau $O(N^2)$.
