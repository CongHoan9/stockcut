// ============================================================================
// STATE — trạng thái dùng chung của ứng dụng (timeline-new.js cũng đọc/ghi object này)
//   Phụ thuộc : (không)
//   Cung cấp  : state
// ============================================================================
// Trạng thái dự án (chỉ còn schema timeline)
let state = {
    config: { aspectRatio: "original" },
    timeline: [],
    virtualClock: null,
    isPlaying: false,
    isExporting: false,          // trước đây là biến toàn cục rời `isExporting` trong main.js
    animationFrameId: null,
    dirHandle: null,
    sessionProjectName: '',
    projectContext: { name: '', sourceType: 'demo', baseUrl: '' },
    configFileHandle: null,
    templateFileHandle: null,
    timelineFileHandle: null,
    styleFileHandle: null,
    lyricsFileHandle: null,
    renderConfig: null,
    currentScale: 1,
    // Bộ đệm lưu trữ trong RAM (In-Memory Buffer) cho các file đang edit
    ram: {
        configJson: "",
        templateJson: "",
        timelineJson: "",
        styleJson: "",
        projectJson: "",
        lyricsTxt: "",
        isJsonDirty: false,
        isLyricsDirty: false
    }
};
if (typeof window !== 'undefined') {
    window.state = state;
}
