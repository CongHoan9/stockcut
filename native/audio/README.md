# Audio Subsystem Architecture (FFT & Bass DSP)

## 1. Tổng quan (Overview)
Thư mục `audio/` xử lý tín hiệu âm thanh kỹ thuật số phục vụ đồng bộ video và hiệu ứng:
- **`audio_dsp.h / .cpp`**: Thực thi biến đổi Fourier nhanh (FFT), trích xuất đường bao năng lượng âm trầm (Bass Energy Envelope), thuật toán phát hiện nhịp điệu (Onset Detection / Superflux), và tạo đệm mẫu hiển thị dạng sóng (Waveform Buffer).

## 2. Điểm nghẽn hệ thống & Hướng khắc phục
- **Điểm nghẽn**: Tính toán phân tích FFT trên JavaScript luồng chính làm giật khung hình render đồ họa.
- **Khắc phục triệt để**:
  - Chuyển 100% thuật toán DSP sang C++ với các lệnh SIMD128 song song.
  - Kết quả phân tích âm lượng bass được nướng thành mảng mẫu tĩnh `RUNTIME_BASS_SAMPLE_CAPACITY` đọc O(1) từ L1 cacheline trong vòng lặp render nóng.
