# Text Subsystem Architecture (FreeType, HarfBuzz & WOFF2)

## 1. Tổng quan (Overview)
Thư mục `text/` chịu trách nhiệm tạo hình, định dạng và dàn trang văn bản cấp thấp:
- **`text_font_engine.h / .cpp`**: Tích hợp FreeType, HarfBuzz và giải nén font WOFF2/Brotli. Quản lý Glyph Atlas Texture 2048x2048 dùng chung cho toàn bộ ứng dụng.
- **`text_layout.h / .cpp`**: Định dạng văn bản đa ngôn ngữ, hỗ trợ Unicode BiDi, ngắt dòng tự động, căn lề và tính toán bounding box từng dòng.
- **`text_reveal.h / .cpp`**: Động cơ hiệu ứng hiển thị ký tự (Text Reveal / Karaoke Lyric Animation) theo thời gian thực (từng ký tự, từng từ, độ mờ đục và offset chuyển động).

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Canvas 2D `fillText` hoặc DOM Text rendering làm chậm tốc độ khung hình và không thể áp dụng shader/instancing.
- **Khắc phục triệt để**:
  - Rasterize glyph 1 lần duy nhất vào GPU Texture Atlas tĩnh.
  - Mỗi ký tự được chuyển thành 1 `VboQuadInstance` 48-byte và render hàng loạt qua instanced draw call tốc độ cao.
