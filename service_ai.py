import os
import sys
import re
import shutil
import tempfile
from typing import List, Optional, Dict, Any
import difflib
import numpy as np

if sys.platform == 'win32':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass
from fastapi import FastAPI, UploadFile, File, Form, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import uvicorn

# ============================================================================
# SERVICE AI — TÁCH VOCAL & TỰ ĐỘNG SINH LYRICS WORD-LEVEL TIMELINE
# ============================================================================

app = FastAPI(
    title="Lyric Video AI Engine",
    description="Engine tách vocal và sinh lyrics chuẩn timeline cho Video Renderer",
    version="1.0.0"
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Global caches
_whisper_cache: Dict[str, Any] = {}
_separator_cache = None

def get_whisper_model(model_size: str = "base"):
    import stable_whisper
    if model_size not in _whisper_cache:
        print(f"[AI] Đang nạp model Stable Faster-Whisper ({model_size}) trên CPU (int8)...")
        _whisper_cache[model_size] = stable_whisper.load_faster_whisper(model_size, device="cpu", compute_type="int8")
        print(f"[AI] Model Stable Faster-Whisper ({model_size}) đã sẵn sàng!")
    return _whisper_cache[model_size]


def separate_audio_stems(audio_path: str, output_dir: str) -> Dict[str, str]:
    """
    Tách âm thanh thành Vocals và Instrumental bằng audio-separator.
    Trả về dict {'vocals': path, 'instrumental': path}.
    """
    from audio_separator.separator import Separator
    os.makedirs(output_dir, exist_ok=True)
    
    print(f"[AI] Bắt đầu tách vocal cho file: {audio_path}")
    separator = Separator(
        output_dir=output_dir,
        output_format="WAV",
        model_file_dir=os.path.join(tempfile.gettempdir(), "audio-separator-models")
    )
    
    # Sử dụng MDX model nhẹ & nhanh trên CPU: Kim_Vocal_2
    try:
        separator.load_model("Kim_Vocal_2.onnx")
    except Exception:
        separator.load_model()

    outputs = separator.separate(audio_path)
    print(f"[AI] Kết quả tách stems: {outputs}")
    
    result = {}
    for filename in outputs:
        full_path = os.path.join(output_dir, filename)
        lower_name = filename.lower()
        # Chú ý: Vì model tên Kim_Vocal_2 nên cả 2 file đều có chữ 'vocal'. Phải phân biệt bằng '(instrumental)' vs '(vocals)'
        if "(instrumental)" in lower_name or "instrumental" in lower_name or "inst" in lower_name:
            result["instrumental"] = full_path
        elif "(vocals)" in lower_name or "vocals" in lower_name or "vocal" in lower_name:
            result["vocals"] = full_path
            
    # Đảm bảo có key vocals và instrumental
    for f in outputs:
        fp = os.path.join(output_dir, f)
        if "instrumental" in f.lower() and "instrumental" not in result:
            result["instrumental"] = fp
        elif "vocal" in f.lower() and "vocals" not in result:
            result["vocals"] = fp

    return result


def clean_word(w: str) -> str:
    """Loại bỏ ký tự đặc biệt, chuẩn hóa từ để so khớp"""
    return re.sub(r'[^\w]', '', w.lower(), flags=re.UNICODE)


def track_vocal_hold(
    audio_data: Optional[np.ndarray],
    sr: int,
    whisper_end: float,
    next_start: Optional[float] = None,
    max_hold: float = 1.8
) -> float:
    """
    Theo dõi năng lượng âm học (RMS) của track giọng hát sau mốc whisper_end
    để phát hiện đoạn ngân nga (sustain / vibrato / vowel hold) của ca sĩ,
    ngăn chặn lyric bị cắt sớm khi ca sĩ vẫn đang ngân giọng.
    """
    if audio_data is None or len(audio_data) == 0:
        return whisper_end

    w_start_idx = max(0, int((whisper_end - 0.25) * sr))
    w_end_idx = int(whisper_end * sr)
    if w_end_idx <= w_start_idx or w_end_idx > len(audio_data):
        return whisper_end

    active_rms = np.sqrt(np.mean(audio_data[w_start_idx:w_end_idx]**2))
    threshold = max(0.015, active_rms * 0.25)

    cur_t = whisper_end
    limit_t = cur_t + max_hold
    if next_start is not None:
        limit_t = min(limit_t, next_start - 0.08)

    step = 0.04
    last_vocal_t = whisper_end
    silence_count = 0

    while cur_t + step <= limit_t:
        s_idx = int(cur_t * sr)
        e_idx = int((cur_t + step) * sr)
        if e_idx > len(audio_data):
            break
        chunk = audio_data[s_idx:e_idx]
        rms = np.sqrt(np.mean(chunk**2))
        if rms >= threshold:
            last_vocal_t = cur_t + step
            silence_count = 0
        else:
            silence_count += 1
            if silence_count >= 3:
                break
        cur_t += step

    return last_vocal_t


def align_lyrics_lines_to_whisper(
    raw_lines: List[str],
    whisper_words: List[Dict[str, Any]],
    audio_path: Optional[str] = None
) -> List[Dict[str, Any]]:
    """
    Căn chỉnh lời bài hát do người dùng nhập vào word-level timestamps từ Whisper AI:
    - 1 DÒNG NGƯỜI DÙNG NHẬP (phân cách bằng \\n) = CHÍNH XÁC 1 BLOCK LYRIC (Không ghép câu lung tung).
    - Giữ nguyên 100% chính tả, chữ hoa/thường, dấu câu của người dùng.
    - Dùng difflib.SequenceMatcher tìm vị trí tương ứng giữa từ của người dùng và từ Whisper nhận diện.
    - Tự động nội suy các từ/câu không nghe rõ hoặc bị khuyết để đảm bảo timeline liên tục, không lỗi.
    - Tích hợp dò quét dải ngân giọng (vocal sustain) từ audio track: giữ câu trọn vẹn đến hết nốt ngân nga.
    - Đảm bảo thời gian bắt đầu và kết thúc giữa các câu mượt mà, không va chạm hay xuất hiện vội vàng.
    """
    valid_lines = [l.strip() for l in raw_lines if l.strip()]
    if not valid_lines:
        return []

    if not whisper_words:
        # Fallback nếu Whisper không nhận được từ nào: tạo timeline nhịp nhàng
        timeline_items = []
        cur_t = 1000
        for l in valid_lines:
            words = [w for w in l.split() if w.strip()]
            dur = max(1500, len(words) * 350)
            kfs = []
            word_dur = dur / max(1, len(words))
            for idx, w in enumerate(words):
                kfs.append({
                    "content": w if idx == 0 else f" {w}",
                    "offset": int(idx * word_dur)
                })
            timeline_items.append({
                "type": "lyric",
                "start": cur_t,
                "end": cur_t + dur,
                "left": "50cqw",
                "top": "50cqh",
                "textAlign": "center",
                "revealMode": "add",
                "transform": "translate(-50%, -50%)",
                "revealDuration": 100,
                "keyframes": kfs
            })
            cur_t += dur + 400
        return timeline_items

    # 1. Tách danh sách token của từng dòng
    line_token_lists = []
    all_user_tokens = []

    for l_idx, line_str in enumerate(valid_lines):
        words_in_line = [w for w in line_str.split() if w.strip()]
        line_tokens = []
        for w_idx, w in enumerate(words_in_line):
            token = {
                "word": w,
                "clean": clean_word(w),
                "line_idx": l_idx,
                "word_idx": w_idx,
                "global_idx": len(all_user_tokens)
            }
            line_tokens.append(token)
            all_user_tokens.append(token)
        line_token_lists.append(line_tokens)

    user_clean = [t["clean"] for t in all_user_tokens]
    ai_clean = [clean_word(w["word"]) for w in whisper_words]

    # 2. Khớp chuỗi từ bằng SequenceMatcher
    matcher = difflib.SequenceMatcher(None, user_clean, ai_clean)
    matches = matcher.get_matching_blocks()

    token_timestamps = [None] * len(all_user_tokens)
    for match in matches:
        for i in range(match.size):
            u_i = match.a + i
            a_i = match.b + i
            token_timestamps[u_i] = (float(whisper_words[a_i]["start"]), float(whisper_words[a_i]["end"]))

    # 3. Nội suy (Interpolation) các từ chưa có timestamp
    last_end = whisper_words[0]["start"] if whisper_words else 0.0
    for i in range(len(all_user_tokens)):
        if token_timestamps[i] is None:
            next_start = None
            for j in range(i + 1, len(all_user_tokens)):
                if token_timestamps[j] is not None:
                    next_start = token_timestamps[j][0]
                    break

            if next_start is not None and next_start > last_end:
                steps = j - i + 1
                alloc_dur = (next_start - last_end) / steps
                start_t = last_end + 0.05
                end_t = min(next_start - 0.05, start_t + alloc_dur * 0.9)
                if end_t <= start_t:
                    end_t = start_t + 0.25
            else:
                start_t = last_end + 0.1
                end_t = start_t + 0.35

            token_timestamps[i] = (round(start_t, 3), round(end_t, 3))

        last_end = max(last_end, token_timestamps[i][1])

    # Nạp audio waveform nếu có để phân tích dải năng lượng ngân giọng
    audio_data = None
    sr = 16000
    if audio_path and os.path.exists(audio_path):
        try:
            from stable_whisper.audio import load_audio
            audio_data = load_audio(audio_path, sr=sr)
        except Exception as load_err:
            print(f"[AI Audio Track] Không thể nạp audio_data ({load_err}), dùng thuật toán nội suy cơ bản.")

    # 4. Sơ bộ tính start_sec & end_sec cho từng line
    raw_blocks = []
    for l_idx, line_tokens in enumerate(line_token_lists):
        if not line_tokens:
            continue

        t_first = token_timestamps[line_tokens[0]["global_idx"]]
        t_last = token_timestamps[line_tokens[-1]["global_idx"]]

        line_start_sec = t_first[0]
        line_end_sec = max(t_last[1], line_start_sec + 0.3)

        raw_blocks.append({
            "tokens": line_tokens,
            "start_sec": line_start_sec,
            "end_sec": line_end_sec
        })

    # 5. Phân tích Vocal Sustain & Khử va chạm/xuất hiện vội vàng giữa các câu
    final_blocks = []
    for i in range(len(raw_blocks)):
        b = raw_blocks[i]
        b_start = b["start_sec"]

        # Ngăn hiện tượng câu mới nhảy ra vội vàng khi câu trước chưa dứt hẳn
        if final_blocks:
            prev_end = final_blocks[-1]["end_sec"]
            if b_start < prev_end + 0.08:
                b_start = prev_end + 0.08

        next_s = raw_blocks[i + 1]["start_sec"] if i + 1 < len(raw_blocks) else None

        # Dò dải năng lượng ngân nga thực tế của ca sĩ sau mốc kết thúc của Whisper
        tracked_end = track_vocal_hold(audio_data, sr, b["end_sec"], next_start=next_s)

        # Tính toán mốc kết thúc hiển thị mượt mà trên video
        if next_s is not None:
            gap = next_s - tracked_end
            if 0 < gap <= 0.65:
                # Nếu khoảng nghỉ ngắn (hát liên tiếp): giữ lyric trên màn hình tới sát câu sau để chuyển cảnh mượt
                final_end = next_s - 0.06
            else:
                # Nếu khoảng nghỉ dài (nghỉ lấy hơi/nhạc dạo): giữ câu thêm 350ms sau khi dứt giọng rồi mới ẩn
                final_end = min(tracked_end + 0.35, next_s - 0.08)
        else:
            final_end = tracked_end + 0.6

        if final_end <= b_start:
            final_end = b_start + 0.4

        final_blocks.append({
            "tokens": b["tokens"],
            "start_ms": int(b_start * 1000),
            "end_ms": int(final_end * 1000),
            "start_sec": b_start,
            "end_sec": final_end
        })

    # 6. Đóng gói thành timeline items chuẩn của renderer
    timeline_items = []
    for blk in final_blocks:
        b_start_ms = blk["start_ms"]
        b_end_ms = blk["end_ms"]
        b_start_sec = blk["start_sec"]

        keyframes = []
        for idx, tok in enumerate(blk["tokens"]):
            w_ts = token_timestamps[tok["global_idx"]]
            offset_ms = 0 if idx == 0 else max(0, int((w_ts[0] - b_start_sec) * 1000))
            content = tok["word"] if idx == 0 else f" {tok['word']}"
            keyframes.append({
                "content": content,
                "offset": offset_ms
            })

        timeline_items.append({
            "type": "lyric",
            "start": b_start_ms,
            "end": b_end_ms,
            "left": "50cqw",
            "top": "50cqh",
            "textAlign": "center",
            "revealMode": "add",
            "transform": "translate(-50%, -50%)",
            "revealDuration": 100,
            "keyframes": keyframes
        })

    return timeline_items


def structure_words_to_timeline(
    words: List[Dict[str, Any]],
    audio_path: Optional[str] = None
) -> List[Dict[str, Any]]:
    """
    Thuật toán đóng gói word timestamps thành các khối Lyric khi người dùng KHÔNG nhập lyric trước:
    - Gom nhóm dựa trên khoảng lặng (gap_sec >= 0.55s) hoặc dấu câu (.?!;).
    - Cho phép câu tự nhiên dài tối đa 9 từ hoặc 4.8 giây trước khi ngắt.
    - Tích hợp track_vocal_hold để giữ nốt ngân nga và khử va chạm/vội vàng giữa các câu.
    - Gán chuẩn các thông số render: type='lyric', revealMode='add', text-align='center'.
    """
    if not words:
        return []

    blocks_of_words: List[List[Dict[str, Any]]] = []
    current_block: List[Dict[str, Any]] = []

    for i, w in enumerate(words):
        word_text = w["word"].strip()
        if not word_text:
            continue

        if not current_block:
            current_block.append(w)
            continue

        prev_w = current_block[-1]
        gap_sec = w["start"] - prev_w["end"]
        block_dur = w["end"] - current_block[0]["start"]
        is_end_punc = bool(re.search(r'[.?!;]$', prev_w["word"].strip()))

        if gap_sec >= 0.55 or len(current_block) >= 9 or block_dur >= 4.8 or is_end_punc:
            blocks_of_words.append(current_block)
            current_block = [w]
        else:
            current_block.append(w)

    if current_block:
        blocks_of_words.append(current_block)

    audio_data = None
    sr = 16000
    if audio_path and os.path.exists(audio_path):
        try:
            from stable_whisper.audio import load_audio
            audio_data = load_audio(audio_path, sr=sr)
        except Exception:
            pass

    timeline_items: List[Dict[str, Any]] = []
    for i, block in enumerate(blocks_of_words):
        if not block:
            continue

        first_w = block[0]
        last_w = block[-1]

        b_start = max(0.0, float(first_w["start"]) - 0.05)
        if timeline_items:
            prev_end = timeline_items[-1]["end"] / 1000.0
            if b_start < prev_end + 0.08:
                b_start = prev_end + 0.08

        next_s = float(blocks_of_words[i + 1][0]["start"]) if i + 1 < len(blocks_of_words) else None
        tracked_end = track_vocal_hold(audio_data, sr, float(last_w["end"]), next_start=next_s)

        if next_s is not None:
            gap = next_s - tracked_end
            if 0 < gap <= 0.65:
                final_end = next_s - 0.06
            else:
                final_end = min(tracked_end + 0.35, next_s - 0.08)
        else:
            final_end = tracked_end + 0.6

        if final_end <= b_start:
            final_end = b_start + 0.4

        block_start_ms = int(b_start * 1000)
        block_end_ms = int(final_end * 1000)

        keyframes = []
        for idx, w in enumerate(block):
            clean_text = w["word"].strip()
            offset_ms = max(0, int((float(w["start"]) - b_start) * 1000))
            content = clean_text if idx == 0 else f" {clean_text}"
            keyframes.append({
                "content": content,
                "offset": offset_ms
            })

        timeline_items.append({
            "type": "lyric",
            "start": block_start_ms,
            "end": block_end_ms,
            "left": "50cqw",
            "top": "50cqh",
            "textAlign": "center",
            "revealMode": "add",
            "transform": "translate(-50%, -50%)",
            "revealDuration": 100,
            "keyframes": keyframes
        })

    return timeline_items


# ============================================================================
# API ENDPOINTS
# ============================================================================

@app.get("/api/status")
async def get_status():
    """Kiểm tra tình trạng hoạt động của service AI"""
    return {
        "status": "ready",
        "cpu_mode": True,
        "whisper": "faster-whisper (CTranslate2 int8)",
        "separator": "audio-separator (ONNX Runtime)"
    }


@app.post("/api/process-lyrics")
async def process_lyrics(
    file: Optional[UploadFile] = File(None),
    audio_path: Optional[str] = Form(None),
    project_dir: Optional[str] = Form(None),
    separate_vocals: bool = Form(True),
    language: Optional[str] = Form(None),
    model_size: str = Form("base"),
    lyrics_text: Optional[str] = Form(None)
):
    """
    Pipeline toàn diện:
    1. Nhận file âm thanh (upload hoặc đường dẫn ổ cứng).
    2. Tách vocal (nếu separate_vocals=True).
    3. Nhận diện giọng nói với Faster-Whisper và lấy word-level timestamps.
    4. Tự động đóng gói thành các item type: "text" chuẩn timeline sẵn sàng render.
    """
    temp_dir = tempfile.mkdtemp(prefix="lyric_ai_")
    source_audio = None

    try:
        # Bước 1: Lưu / xác định file âm thanh nguồn
        if file and hasattr(file, "filename") and file.filename:
            source_audio = os.path.join(temp_dir, file.filename)
            with open(source_audio, "wb") as f:
                shutil.copyfileobj(file.file, f)
        elif audio_path and os.path.exists(audio_path):
            source_audio = audio_path
        else:
            raise HTTPException(status_code=400, detail="Vui lòng cung cấp file upload hoặc audio_path hợp lệ.")

        # Thư mục xuất file tách stems
        output_stems_dir = project_dir if (project_dir and os.path.exists(project_dir)) else temp_dir

        target_transcribe_audio = source_audio
        vocal_file_path = None
        inst_file_path = None

        # Bước 2: Tách vocal nếu được bật
        if separate_vocals:
            try:
                stems = separate_audio_stems(source_audio, output_stems_dir)
                if "vocals" in stems and os.path.exists(stems["vocals"]):
                    target_transcribe_audio = stems["vocals"]
                    vocal_file_path = stems["vocals"]
                if "instrumental" in stems:
                    inst_file_path = stems["instrumental"]
            except Exception as e:
                print(f"[AI Cảnh báo] Tách vocal gặp lỗi ({e}), chuyển sang nhận diện trực tiếp trên file gốc.")

        # Bước 3 & 4: Xử lý theo chế độ FORCED ALIGNMENT (khi có lyrics_text) hoặc TRANSCRIBE STABLE (khi không có)
        whisper = get_whisper_model(model_size)
        detected_lang = language or "auto"
        lang_prob = 1.0
        full_text = []

        # Tự động xác định ngôn ngữ nếu người dùng chọn "auto" hoặc không truyền
        target_lang = None
        if language and language.strip() and language.strip().lower() != "auto":
            target_lang = language.strip().lower()
        else:
            try:
                # Dò nhanh ngôn ngữ từ file âm thanh bằng transcribe_original (chỉ tính info header, cực nhanh)
                _, detect_info = whisper.transcribe_original(target_transcribe_audio, vad_filter=True)
                target_lang = detect_info.language
                lang_prob = round(float(detect_info.language_probability), 3) if detect_info.language_probability else 1.0
                print(f"[AI] Tự động phát hiện ngôn ngữ: '{target_lang}' (độ tin cậy: {lang_prob})")
            except Exception as e_det:
                print(f"[AI] Không thể tự động phát hiện ngôn ngữ ({e_det}), mặc định sử dụng 'en'")
                target_lang = "en"
        detected_lang = target_lang

        if lyrics_text and lyrics_text.strip():
            raw_lines = [l.strip() for l in lyrics_text.splitlines() if l.strip()]
            full_clean_text = "\n".join(raw_lines)
            full_text = [full_clean_text]

            align_args = {
                "original_split": True,
                "max_word_dur": 6.0,
                "word_dur_factor": 4.0,
                "nonspeech_skip": 5.0,
                "language": target_lang
            }

            try:
                print(f"[AI Forced Alignment] Đang chạy căn chỉnh âm học trực tiếp cho {len(raw_lines)} dòng lời (ngôn ngữ: {target_lang})...")
                align_res = whisper.align(target_transcribe_audio, full_clean_text, **align_args)

                aligned_words = []
                for seg in align_res.segments:
                    for w in seg.words:
                        w_clean = w.word.strip()
                        if w_clean:
                            aligned_words.append({
                                "word": w_clean,
                                "start": float(w.start),
                                "end": float(w.end),
                                "probability": float(getattr(w, "probability", 1.0))
                            })

                timeline_items = align_lyrics_lines_to_whisper(raw_lines, aligned_words, audio_path=target_transcribe_audio)

            except Exception as align_err:
                print(f"[AI Alignment Fallback] Forced alignment gặp lỗi ({align_err}), chuyển sang transcribe fallback...")
                transcribe_args = {}
                if target_lang:
                    transcribe_args["language"] = target_lang
                whisper_result = whisper.transcribe(target_transcribe_audio, **transcribe_args)
                detected_lang = getattr(whisper_result, "language", target_lang)
                all_words = []
                full_text = []
                for seg in whisper_result.segments:
                    full_text.append(seg.text.strip())
                    if hasattr(seg, "words") and seg.words:
                        for w in seg.words:
                            w_str = str(w.word).strip()
                            if w_str:
                                all_words.append({
                                    "word": w_str,
                                    "start": float(w.start),
                                    "end": float(w.end),
                                    "probability": float(getattr(w, "probability", 1.0))
                                })
                timeline_items = align_lyrics_lines_to_whisper(raw_lines, all_words, audio_path=target_transcribe_audio)
        else:
            # Người dùng không cung cấp lyrics: Chạy nhận diện giọng nói tự do
            transcribe_args = {}
            if target_lang:
                transcribe_args["language"] = target_lang
            whisper_result = whisper.transcribe(target_transcribe_audio, **transcribe_args)
            detected_lang = getattr(whisper_result, "language", target_lang)
            all_words = []
            full_text = []
            for seg in whisper_result.segments:
                full_text.append(seg.text.strip())
                if hasattr(seg, "words") and seg.words:
                    for w in seg.words:
                        w_str = str(w.word).strip()
                        if w_str:
                            all_words.append({
                                "word": w_str,
                                "start": float(w.start),
                                "end": float(w.end),
                                "probability": float(getattr(w, "probability", 1.0))
                            })
            timeline_items = structure_words_to_timeline(all_words, audio_path=target_transcribe_audio)

        return {
            "success": True,
            "detected_language": detected_lang,
            "language_probability": lang_prob,
            "total_items": len(timeline_items),
            "full_text": " ".join(full_text),
            "timeline_items": timeline_items,
            "vocals_path": vocal_file_path,
            "instrumental_path": inst_file_path
        }

    except Exception as e:
        import traceback
        traceback.print_exc()
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        # Nếu dùng temp file và không cần giữ, dọn dẹp
        pass


if __name__ == "__main__":
    print("======================================================")
    print("Khởi động Lyric Video AI Service tại http://127.0.0.1:8000")
    print("Sẵn sàng phục vụ yêu cầu Tách Vocal & Sinh Lyric Timeline!")
    print("======================================================")
    uvicorn.run(app, host="127.0.0.1", port=8000, log_level="info")
