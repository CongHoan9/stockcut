# Cache Subsystem Architecture (Transposition & Zobrist)

## 1. Tổng quan (Overview)
Thư mục `cache/` áp dụng các kỹ thuật cấu trúc dữ liệu tiên tiến lấy cảm hứng từ Stockfish:
- **`transposition_table.h / .cpp`**: Bảng hoán vị (Transposition Table) 2-bucket với cơ chế phân rã thế hệ (generational aging) và truy xuất không khóa. Cho phép tái sử dụng kết quả render khung hình (Pre-rendered Frame Cache Reuse) O(1).
- **`zobrist.h / .cpp`**: Mã băm Zobrist 64-bit sử dụng bảng khóa ngẫu nhiên tĩnh. Khi trạng thái timeline biến đổi (thời gian, keyframe, style, layer), mã băm được cập nhật vi sai qua phép toán XOR siêu tốc (`^=`).
- **`computation_cache.h / .cpp`**: Bộ nhớ đệm lưu trữ các phép tính nặng (ma trận biến đổi, layout text, đường cong audio).

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Tính toán lại toàn bộ khung hình giống nhau hoặc chỉ đổi rất ít thuộc tính gây lãng phí 95% chu kỳ render.
- **Khắc phục triệt để**:
  - `Zobrist Breaker Skip`: Nếu mã băm Zobrist của khung hình hiện tại trùng khớp với khung hình trước và không có hiệu ứng liên tục, C++ engine bỏ qua hoàn toàn việc tính toán GPU (0 draw calls, 0 allocation), bảo toàn FPS tối đa.
  - Phép băm XOR vi sai O(1) thay vì băm lại toàn bộ cây DOM/JSON.
