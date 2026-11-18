# Render Subsystem Architecture (Native OpenGL ES 3.0)

## 1. Tổng quan (Overview)
Thư mục `render/` là trung tâm thực thi đồ họa cấp thấp của toàn bộ hệ thống:
- **`native_renderer.h / .cpp`**: Quản lý VAO, VBO, FBO, Shader Programs và phát lệnh trực tiếp tới OpenGL ES 3.0 (<GLES3/gl3.h>). Thực thi các render pass phức tạp:
  - HDR Bloom Mip Pyramid (13-Tap Karis Extract, 13-Tap Jimenez Downsample, 9-Tap Progressive Upsample, Jodie-Reinhard Tonemap, IGN Stochastic Dither).
  - Multi-effect shader dispatch (Matrix, Glitch, Water, Fire, Twirl, v.v.).
  - Blit khung cảnh tới framebuffer màn hình chính (Double Buffering).
  - Tự động đo đạc thời gian render thực tế (renderMs) và tính toán Uncapped Throughput FPS.
- **`render_layer.h`**: Quản lý bộ nhớ VBO theo trang (`RenderLayer<RenderPolicy::StaticChunked>` và `RenderLayer<RenderPolicy::DynamicDense>`), đồng bộ với `GPUTransferQueue`.

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: JS can thiệp vào khâu render gây mất dữ liệu dynamic range (8-bit banding), nghẽn trần 60 FPS do VSync View.
- **Khắc phục triệt để**:
  - Render 100% nội bộ C++ Native OpenGL ES, loại bỏ hoàn toàn các hàm fallback JS.
  - Đo FPS trực tiếp từ thời gian hoàn thành lệnh đồ họa trong C++, hiển thị throughput FPS thực sự trên Dashboard.
