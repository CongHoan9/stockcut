# Thư mục Templates (.hbs)

Thư mục này chứa toàn bộ các file template Handlebars (.hbs) được sử dụng để đổ dữ liệu giao diện có phong cách lặp lại trong ứng dụng.

## Danh sách khuôn mẫu:
1. `media-card.hbs`: Thẻ hiển thị file Media (hỗ trợ cả Video và Image) trong tab Media Drawer.
2. `media-audio-item.hbs`: Hàng hiển thị file âm thanh (Audio) trong danh sách nhạc với nút phát thử, tên, định dạng, tác giả, đường dẫn và thời lượng.
3. `font-item.hbs`: Phần tử hiển thị font chữ trong tab Font ở panel bên trái.
4. `effect-item.hbs`: Thẻ hiển thị hiệu ứng timeline (Zoom, Shake, Glitch, Matrix,...) trong tab Effect.
5. `spec-item.hbs`: Hàng thông số kỹ thuật (Độ phân giải, FPS, Thuật toán, Bitrate,...) trong modal xuất video.

## Cách sử dụng qua `Templates` Engine:
```javascript
// Render ra chuỗi HTML
const html = Templates.render('media-card', { name: 'Sample', isVideo: true, ... });

// Render trực tiếp ra HTMLElement
const el = Templates.renderElement('media-card', { name: 'Sample', isVideo: true, ... });
```
