"""Local HTTP & Direct-Disk Server with ~/.stockcut Anchor Registry, ETag Caching, and Range Streaming.

Architecture:
1. User Machine = Personal Server:
   - Anchors persistent metadata at `C:/Users/<User>/.stockcut/` (and `C:/.stockcut/anchor.json` pointer):
     * `anchor.json`      : Root anchor paths and environment metadata
     * `workspace.json`   : Active project path, playhead position, aspect ratio
     * `projects.json`    : Absolute paths to all projects (`config.json`, `template.json`, `timeline.json`, `style.json`, `lyrics.txt`)
     * `media_links.json` : Absolute paths to all user-added video & image files
     * `audio_links.json` : Absolute paths to all user-added music & audio files
     * `text_links.json`  : Absolute paths to all user text & lyrics files (`lyrics.txt`, `.lrc`, `.srt`)
2. Built-in Cloud Assets (`Asset/`) vs Local User Disk (`/@fs/C:/...`):
   - Files in `Asset/` are built-in static assets (`/Asset/...`) ready for GitHub Pages / Render deployment.
   - User files are accessed directly from disk by absolute path via `/@fs/C:/...` with HTTP 206 Range streaming (Zero RAM preload).
"""
import http.server
import json
import os
import socketserver
import sys
import time
import urllib.parse
from functools import partial
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
STOCKCUT_DIR = Path.home() / ".stockcut"

VIDEO_EXTS = {'.mp4', '.webm', '.mov', '.mkv', '.avi', '.m4v'}
IMAGE_EXTS = {'.png', '.jpg', '.jpeg', '.webp', '.gif', '.svg', '.bmp'}
AUDIO_EXTS = {'.mp3', '.wav', '.m4a', '.ogg', '.flac', '.aac', '.wma'}
TEXT_EXTS  = {'.txt', '.lrc', '.srt', '.vtt'}


def norm_abs(p) -> str:
    return str(Path(p).resolve()).replace('\\', '/')


def read_json_safe(path: Path, default):
    try:
        if path.exists():
            with open(path, 'r', encoding='utf-8') as f:
                return json.load(f)
    except Exception:
        pass
    return default


def write_json_atomic(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


class StockCutDiskRegistry:
    """Manages ~/.stockcut persistent link registry on the user's disk (Zero RAM bloat)."""

    def __init__(self, root_dir: Path, stockcut_dir: Path):
        self.root_dir = root_dir
        self.dir = stockcut_dir
        self.dir.mkdir(parents=True, exist_ok=True)

        # Standard commercial application directory hierarchy (.stockcut/)
        self.config_dir = self.dir / "config"
        self.registry_dir = self.dir / "registry"
        self.projects_dir = self.dir / "Projects"
        self.cache_dir = self.dir / "cache"
        self.waveforms_cache_dir = self.cache_dir / "waveforms"
        self.thumbnails_cache_dir = self.cache_dir / "thumbnails"
        self.shaders_cache_dir = self.cache_dir / "shaders"
        self.logs_dir = self.dir / "logs"

        for d in (
            self.config_dir,
            self.registry_dir,
            self.projects_dir,
            self.cache_dir,
            self.waveforms_cache_dir,
            self.thumbnails_cache_dir,
            self.shaders_cache_dir,
            self.logs_dir,
        ):
            d.mkdir(parents=True, exist_ok=True)

        self.anchor_file = self.dir / "anchor.json"
        self.settings_file = self.config_dir / "settings.json"
        self.workspace_file = self.config_dir / "workspace.json"
        self.recent_file = self.config_dir / "recent.json"

        self.projects_file = self.registry_dir / "projects.json"
        self.media_file = self.registry_dir / "media_links.json"
        self.audio_file = self.registry_dir / "audio_links.json"
        self.text_file = self.registry_dir / "text_links.json"
        self.fonts_file = self.registry_dir / "font_links.json"
        self.cloud_assets_file = self.registry_dir / "cloud_assets.json"
        self.log_file = self.logs_dir / "engine.log"

        # Clean up any legacy flat files in root of .stockcut/ after migrating
        self._migrate_legacy_flat_files()

        self._file_index_cache = {}
        self._index_built = False
        self.ensure_initialized()

    def _migrate_legacy_flat_files(self):
        migrations = [
            (self.dir / "workspace.json", self.workspace_file),
            (self.dir / "projects.json", self.projects_file),
            (self.dir / "media_links.json", self.media_file),
            (self.dir / "audio_links.json", self.audio_file),
            (self.dir / "text_links.json", self.text_file),
        ]
        for old_p, new_p in migrations:
            if old_p.exists():
                try:
                    if not new_p.exists():
                        old_p.replace(new_p)
                    else:
                        old_p.unlink()
                except Exception:
                    pass

    def _ensure_c_drive_junction(self):
        c_dir = Path("C:/.stockcut")
        try:
            if c_dir.resolve() == self.dir.resolve():
                return
        except Exception:
            pass
        try:
            import shutil
            import subprocess
            if c_dir.exists() and not c_dir.is_symlink():
                # If C:/.stockcut only had anchor.json, replace it with a real directory junction to ~/.stockcut
                children = list(c_dir.iterdir())
                if len(children) <= 1:
                    shutil.rmtree(c_dir, ignore_errors=True)
            if not c_dir.exists():
                subprocess.run(
                    ["cmd", "/c", "mklink", "/J", str(c_dir), str(self.dir)],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
        except Exception:
            pass

    def _append_log(self, event: str, detail: str = ""):
        try:
            ts = time.strftime("%Y-%m-%d %H:%M:%S")
            line = f"[{ts}] {event} | {detail}\n"
            with open(self.log_file, "a", encoding="utf-8") as f:
                f.write(line)
        except Exception:
            pass

    def ensure_initialized(self):
        now_ms = int(time.time() * 1000)
        self._ensure_c_drive_junction()

        anchor_data = {
            "app": "StockCut Studio Engine",
            "version": 2,
            "stockcutDir": norm_abs(self.dir),
            "cDriveMount": "C:/.stockcut",
            "workspaceRoot": norm_abs(self.root_dir),
            "builtinAssetPrefix": "/Asset/",
            "localFsPrefix": "/@fs/",
            "directories": {
                "config": norm_abs(self.config_dir),
                "registry": norm_abs(self.registry_dir),
                "projects": norm_abs(self.projects_dir),
                "cache": norm_abs(self.cache_dir),
                "waveformsCache": norm_abs(self.waveforms_cache_dir),
                "thumbnailsCache": norm_abs(self.thumbnails_cache_dir),
                "shadersCache": norm_abs(self.shaders_cache_dir),
                "logs": norm_abs(self.logs_dir),
            },
            "files": {
                "settings": norm_abs(self.settings_file),
                "workspace": norm_abs(self.workspace_file),
                "recent": norm_abs(self.recent_file),
                "projects": norm_abs(self.projects_file),
                "media": norm_abs(self.media_file),
                "audio": norm_abs(self.audio_file),
                "text": norm_abs(self.text_file),
                "fonts": norm_abs(self.fonts_file),
                "cloudAssets": norm_abs(self.cloud_assets_file),
                "engineLog": norm_abs(self.log_file),
            },
            "updatedAt": now_ms,
        }
        write_json_atomic(self.anchor_file, anchor_data)

        if not self.settings_file.exists():
            write_json_atomic(self.settings_file, {
                "engine": "StockCut WASM SIMD + WebGL2",
                "targetFps": 60,
                "zeroRamPreload": True,
                "directDiskStream": True,
                "httpRangeStreaming": True,
                "defaultAspectRatio": "original",
                "autoSaveOnEdit": True,
                "updatedAt": now_ms,
            })

        # Sync built-in cloud assets (Asset/manifest.json) & fonts (Asset/font/fonts.json)
        manifest_data = read_json_safe(self.root_dir / "Asset" / "manifest.json", {})
        write_json_atomic(self.cloud_assets_file, {
            "scope": "builtin",
            "cdnBaseUrl": "/Asset",
            "deployTargets": ["github-pages", "render-static"],
            "manifest": manifest_data,
            "updatedAt": now_ms,
        })

        fonts_manifest = read_json_safe(self.root_dir / "Asset" / "font" / "fonts.json", [])
        font_items = []
        if isinstance(fonts_manifest, list):
            for f_entry in fonts_manifest:
                if not isinstance(f_entry, dict):
                    continue
                fam = f_entry.get("family", "")
                f_dir = f_entry.get("dir", fam)
                variants = f_entry.get("variants", [])
                files = []
                for v in variants:
                    if isinstance(v, dict) and v.get("file"):
                        raw_f = v["file"].lstrip("/")
                        rel_f = raw_f if raw_f.startswith("Asset/font/") else f"Asset/font/{f_dir}/{raw_f}"
                        files.append({
                            "weight": v.get("weight", 400),
                            "italic": bool(v.get("italic", False)),
                            "cloudUrl": "/" + rel_f,
                            "absPath": norm_abs(self.root_dir / rel_f),
                        })
                font_items.append({
                    "family": fam,
                    "dir": f_dir,
                    "scope": "builtin",
                    "variantCount": len(files),
                    "variants": files,
                })
        write_json_atomic(self.fonts_file, {
            "totalFamilies": len(font_items),
            "items": font_items,
            "updatedAt": now_ms,
        })

        # Ensure cache index files exist
        if not (self.waveforms_cache_dir / "index.json").exists():
            write_json_atomic(self.waveforms_cache_dir / "index.json", {"entries": {}, "updatedAt": now_ms})
        if not (self.thumbnails_cache_dir / "index.json").exists():
            write_json_atomic(self.thumbnails_cache_dir / "index.json", {"entries": {}, "updatedAt": now_ms})
        if not (self.shaders_cache_dir / "pipeline_state.json").exists():
            write_json_atomic(self.shaders_cache_dir / "pipeline_state.json", {
                "wasmBinary": norm_abs(self.root_dir / "js" / "stockcut.wasm"),
                "simdEnabled": True,
                "updatedAt": now_ms,
            })

        projects = read_json_safe(self.projects_file, {"items": []})
        if not isinstance(projects, dict) or "items" not in projects:
            projects = {"items": []}

        media_links = read_json_safe(self.media_file, {"items": []})
        if not isinstance(media_links, dict) or "items" not in media_links:
            media_links = {"items": []}

        audio_links = read_json_safe(self.audio_file, {"items": []})
        if not isinstance(audio_links, dict) or "items" not in audio_links:
            audio_links = {"items": []}

        text_links = read_json_safe(self.text_file, {"items": []})
        if not isinstance(text_links, dict) or "items" not in text_links:
            text_links = {"items": []}

        # Scan local projects in src/ once to populate absolute links & per-project manifests in Projects/<Name>/
        src_dir = self.root_dir / "src"
        existing_proj_paths = {item.get("rootPath") for item in projects["items"] if isinstance(item, dict)}
        existing_text_paths = {item.get("path") for item in text_links["items"] if isinstance(item, dict)}
        existing_media_paths = {item.get("path") for item in media_links["items"] if isinstance(item, dict)}
        existing_audio_paths = {item.get("path") for item in audio_links["items"] if isinstance(item, dict)}

        changed_projects = not self.projects_file.exists()
        changed_texts = not self.text_file.exists()
        changed_media = not self.media_file.exists()
        changed_audio = not self.audio_file.exists()

        if src_dir.is_dir():
            for child in sorted(src_dir.iterdir()):
                if not child.is_dir():
                    continue
                cfg_p = child / "config.json"
                tl_p = child / "timeline.json"
                if cfg_p.exists() and tl_p.exists():
                    abs_root = norm_abs(child)
                    proj_entry = self._build_project_entry(child)
                    self._write_project_folder_manifest(child, proj_entry)
                    if abs_root not in existing_proj_paths:
                        projects["items"].append(proj_entry)
                        existing_proj_paths.add(abs_root)
                        changed_projects = True

                    lyrics_p = child / "lyrics.txt"
                    if lyrics_p.exists():
                        abs_l = norm_abs(lyrics_p)
                        if abs_l not in existing_text_paths:
                            text_links["items"].append({
                                "id": f"txt_{child.name}",
                                "name": "lyrics.txt",
                                "project": child.name,
                                "absPath": abs_l,
                                "path": abs_l,
                                "streamUrl": f"/@fs/{abs_l}",
                                "size": lyrics_p.stat().st_size,
                                "scope": "local",
                                "updatedAt": now_ms,
                            })
                            existing_text_paths.add(abs_l)
                            changed_texts = True

                    # Register any local media/audio inside project folder by absolute link
                    for f in child.rglob("*"):
                        if not f.is_file():
                            continue
                        ext = f.suffix.lower()
                        abs_f = norm_abs(f)
                        if ext in VIDEO_EXTS or ext in IMAGE_EXTS:
                            if abs_f not in existing_media_paths:
                                mtype = "video" if ext in VIDEO_EXTS else "image"
                                media_links["items"].append({
                                    "id": f"local_{mtype}_{f.name}",
                                    "name": f.name,
                                    "absPath": abs_f,
                                    "path": abs_f,
                                    "src": abs_f,
                                    "streamUrl": f"/@fs/{abs_f}",
                                    "type": mtype,
                                    "size": f.stat().st_size,
                                    "project": child.name,
                                    "scope": "local",
                                    "isAsset": False,
                                    "addedAt": now_ms,
                                })
                                existing_media_paths.add(abs_f)
                                changed_media = True
                        elif ext in AUDIO_EXTS:
                            if abs_f not in existing_audio_paths:
                                audio_links["items"].append({
                                    "id": f"local_audio_{f.name}",
                                    "name": f.name,
                                    "title": f.stem,
                                    "artist": child.name,
                                    "absPath": abs_f,
                                    "path": abs_f,
                                    "src": abs_f,
                                    "streamUrl": f"/@fs/{abs_f}",
                                    "type": "audio",
                                    "size": f.stat().st_size,
                                    "project": child.name,
                                    "scope": "local",
                                    "isAsset": False,
                                    "addedAt": now_ms,
                                })
                                existing_audio_paths.add(abs_f)
                                changed_audio = True

        if changed_projects:
            write_json_atomic(self.projects_file, projects)
        if changed_media:
            write_json_atomic(self.media_file, media_links)
        if changed_audio:
            write_json_atomic(self.audio_file, audio_links)
        if changed_texts:
            write_json_atomic(self.text_file, text_links)

        if not self.workspace_file.exists():
            default_proj = self.root_dir / "src" / "Hotel Room"
            if not default_proj.exists() and projects["items"]:
                default_proj = Path(projects["items"][0]["rootPath"])
            ws = {
                "activeProjectName": default_proj.name,
                "activeProjectPath": norm_abs(default_proj),
                "playheadMs": 0,
                "aspectRatio": "original",
                "updatedAt": now_ms,
            }
            write_json_atomic(self.workspace_file, ws)

        if not self.recent_file.exists():
            ws_cur = read_json_safe(self.workspace_file, {})
            write_json_atomic(self.recent_file, {
                "recentProjects": [
                    {
                        "name": it.get("name"),
                        "rootPath": it.get("rootPath"),
                        "manifestPath": it.get("manifestPath"),
                        "lastOpenedAt": now_ms,
                    }
                    for it in projects["items"][:10]
                ],
                "activeProjectName": ws_cur.get("activeProjectName", "Hotel Room"),
                "updatedAt": now_ms,
            })

        self._append_log("INIT_COMPLETE", f"projects={len(projects['items'])} media={len(media_links['items'])} audio={len(audio_links['items'])} text={len(text_links['items'])}")

    def _build_project_entry(self, folder: Path) -> dict:
        proj_box_dir = self.projects_dir / folder.name
        manifest_path = proj_box_dir / "project.stockcut.json"
        return {
            "name": folder.name,
            "rootPath": norm_abs(folder),
            "manifestPath": norm_abs(manifest_path),
            "configPath": norm_abs(folder / "config.json"),
            "templatePath": norm_abs(folder / "template.json"),
            "timelinePath": norm_abs(folder / "timeline.json"),
            "stylePath": norm_abs(folder / "style.json"),
            "lyricsPath": norm_abs(folder / "lyrics.txt"),
            "scope": "local",
            "updatedAt": int(time.time() * 1000),
        }

    def _write_project_folder_manifest(self, folder: Path, entry: dict):
        proj_box_dir = self.projects_dir / folder.name
        proj_box_dir.mkdir(parents=True, exist_ok=True)
        tl = read_json_safe(folder / "timeline.json", [])
        cfg = read_json_safe(folder / "config.json", {})
        local_links = []
        cloud_links = []
        fonts_used = set()

        def walk_items(items):
            if not isinstance(items, list):
                return
            for it in items:
                if not isinstance(it, dict):
                    continue
                src = it.get("src") or ""
                if isinstance(src, str) and src:
                    clean_s = src.replace("\\", "/").lstrip("./")
                    if clean_s.startswith("/Asset/") or clean_s.startswith("Asset/"):
                        cloud_url = "/" + clean_s.lstrip("/")
                        if cloud_url not in cloud_links:
                            cloud_links.append(cloud_url)
                    else:
                        cand = folder / clean_s
                        abs_s = norm_abs(cand) if cand.exists() else clean_s
                        if abs_s not in local_links:
                            local_links.append(abs_s)
                for sub_key in ("fontFamily", "font"):
                    if isinstance(it.get(sub_key), str) and it.get(sub_key):
                        fonts_used.add(it[sub_key])
                for style_obj in (it.get("css"), it.get("style"), it.get("params")):
                    if isinstance(style_obj, dict):
                        ff = style_obj.get("fontFamily") or style_obj.get("font-family")
                        if isinstance(ff, str) and ff:
                            fonts_used.add(ff)
                walk_items(it.get("children"))
                walk_items(it.get("childrent"))

        walk_items(tl)
        manifest_payload = {
            **entry,
            "aspectRatio": cfg.get("aspectRatio", "original") if isinstance(cfg, dict) else "original",
            "timelineItemCount": len(tl) if isinstance(tl, list) else 0,
            "dependencies": {
                "localDiskLinks": local_links,
                "cloudAssetLinks": cloud_links,
                "fontFamilies": sorted(fonts_used),
            },
        }
        write_json_atomic(proj_box_dir / "project.stockcut.json", manifest_payload)

    def read_project_payload(self, project_path_or_name: str):
        if not project_path_or_name:
            return None
        folder = Path(project_path_or_name)
        if not folder.is_Absolute() if hasattr(folder, 'is_Absolute') else not folder.is_absolute():
            candidate = self.root_dir / "src" / project_path_or_name
            if candidate.is_dir():
                folder = candidate
            else:
                candidate2 = self.root_dir / project_path_or_name
                if candidate2.is_dir():
                    folder = candidate2

        if not folder.is_dir():
            # Search in projects.json by name
            projects = read_json_safe(self.projects_file, {"items": []})
            for item in projects.get("items", []):
                if item.get("name") == project_path_or_name or item.get("rootPath") == project_path_or_name:
                    p = Path(item.get("rootPath", ""))
                    if p.is_dir():
                        folder = p
                        break

        if not folder.is_dir():
            return None

        cfg = read_json_safe(folder / "config.json", {"aspectRatio": "original"})
        if isinstance(cfg, dict) and "lyricSize" in cfg:
            del cfg["lyricSize"]
        tmpl = read_json_safe(folder / "template.json", [])
        tl = read_json_safe(folder / "timeline.json", [])
        st = read_json_safe(folder / "style.json", [])
        lyrics_txt = ""
        lyrics_file = folder / "lyrics.txt"
        if lyrics_file.exists():
            try:
                lyrics_txt = lyrics_file.read_text(encoding='utf-8')
            except Exception:
                lyrics_txt = ""

        entry = self._build_project_entry(folder)
        self.register_project(entry)
        proj_data = {
            "config": cfg if isinstance(cfg, dict) else {"aspectRatio": "original"},
            "template": tmpl if isinstance(tmpl, list) else [],
            "timeline": tl if isinstance(tl, list) else [],
            "style": st if isinstance(st, list) else [],
        }
        return {
            "name": folder.name,
            "rootPath": norm_abs(folder),
            "baseUrl": f"/@fs/{norm_abs(folder)}",
            "files": entry,
            "config": proj_data["config"],
            "template": proj_data["template"],
            "timeline": proj_data["timeline"],
            "style": proj_data["style"],
            "projectData": proj_data,
            "lyricsText": lyrics_txt,
            "lyricsTxt": lyrics_txt,
        }

    def save_project_payload(self, payload: dict) -> dict:
        root_path = payload.get("rootPath") or payload.get("path")
        name = payload.get("name") or "Project"
        if not root_path:
            ws = read_json_safe(self.workspace_file, {})
            root_path = ws.get("activeProjectPath")
        if not root_path:
            root_path = str(self.root_dir / "src" / name)

        folder = Path(root_path)
        if not folder.is_absolute():
            folder = (self.root_dir / root_path).resolve()
        folder.mkdir(parents=True, exist_ok=True)

        proj_data = payload.get("projectData")
        if not proj_data and isinstance(payload.get("projectJson"), str):
            try:
                proj_data = json.loads(payload["projectJson"])
            except Exception:
                proj_data = None

        saved_files = []
        if isinstance(proj_data, dict):
            if "config" in proj_data:
                write_json_atomic(folder / "config.json", proj_data["config"])
                saved_files.append(norm_abs(folder / "config.json"))
            if "template" in proj_data:
                write_json_atomic(folder / "template.json", proj_data["template"])
                saved_files.append(norm_abs(folder / "template.json"))
            if "timeline" in proj_data:
                write_json_atomic(folder / "timeline.json", proj_data["timeline"])
                saved_files.append(norm_abs(folder / "timeline.json"))
            if "style" in proj_data:
                write_json_atomic(folder / "style.json", proj_data["style"])
                saved_files.append(norm_abs(folder / "style.json"))

        if isinstance(payload.get("lyricsTxt"), str):
            lyrics_file = folder / "lyrics.txt"
            lyrics_file.write_text(payload["lyricsTxt"], encoding='utf-8')
            saved_files.append(norm_abs(lyrics_file))

        entry = self._build_project_entry(folder)
        self._write_project_folder_manifest(folder, entry)
        self.register_project(entry)
        self.update_workspace({
            "activeProjectName": folder.name,
            "activeProjectPath": norm_abs(folder),
            "playheadMs": payload.get("playheadMs", 0),
        })
        self._append_log("PROJECT_SAVE", f"name={folder.name} root={norm_abs(folder)} files={len(saved_files)}")
        return {"ok": True, "rootPath": norm_abs(folder), "savedFiles": saved_files}

    def register_project(self, entry: dict):
        now_ms = int(time.time() * 1000)
        projects = read_json_safe(self.projects_file, {"items": []})
        items = [it for it in projects.get("items", []) if it.get("rootPath") != entry.get("rootPath")]
        items.insert(0, entry)
        projects["items"] = items
        write_json_atomic(self.projects_file, projects)
        write_json_atomic(self.recent_file, {
            "recentProjects": [
                {
                    "name": it.get("name"),
                    "rootPath": it.get("rootPath"),
                    "manifestPath": it.get("manifestPath"),
                    "lastOpenedAt": now_ms,
                }
                for it in items[:10]
            ],
            "activeProjectName": entry.get("name", ""),
            "updatedAt": now_ms,
        })

    def update_workspace(self, updates: dict) -> dict:
        ws = read_json_safe(self.workspace_file, {})
        ws.update(updates)
        ws["updatedAt"] = int(time.time() * 1000)
        write_json_atomic(self.workspace_file, ws)
        return ws

    def add_link(self, item: dict) -> dict:
        if not isinstance(item, dict):
            return {"ok": False}
        raw_path = item.get("absPath") or item.get("path") or item.get("src") or ""
        if not raw_path:
            return {"ok": False}
        clean_path = raw_path.replace('\\', '/')
        ext = Path(clean_path).suffix.lower()
        inferred_type = "video"
        if ext in AUDIO_EXTS:
            inferred_type = "audio"
        elif ext in IMAGE_EXTS:
            inferred_type = "image"
        elif ext in TEXT_EXTS:
            inferred_type = "text"
        mtype = (item.get("type") or inferred_type).lower()
        now_ms = int(time.time() * 1000)

        record = {
            "id": item.get("id") or f"link_{mtype}_{now_ms}",
            "name": item.get("name") or clean_path.split('/')[-1],
            "absPath": clean_path,
            "path": clean_path,
            "src": clean_path,
            "type": mtype,
            "size": item.get("size", 0),
            "duration": item.get("duration", 0),
            "title": item.get("title") or item.get("name") or clean_path.split('/')[-1],
            "artist": item.get("artist") or "",
            "thumb": item.get("thumb"),
            "scope": "builtin" if clean_path.startswith("/Asset/") or clean_path.startswith("Asset/") else "local",
            "isAsset": clean_path.startswith("/Asset/") or clean_path.startswith("Asset/"),
            "addedAt": item.get("addedAt") or now_ms,
        }

        target_file = self.media_file
        if mtype == "audio":
            target_file = self.audio_file
        elif mtype in ("text", "lyric", "lyrics"):
            target_file = self.text_file

        data = read_json_safe(target_file, {"items": []})
        items = [x for x in data.get("items", []) if x.get("path") != clean_path]
        items.insert(0, record)
        data["items"] = items
        write_json_atomic(target_file, data)
        return {"ok": True, "item": record}

    def remove_link(self, path_or_id: str) -> dict:
        if not path_or_id:
            return {"ok": False}
        for target_file in (self.media_file, self.audio_file, self.text_file):
            data = read_json_safe(target_file, {"items": []})
            before = len(data.get("items", []))
            data["items"] = [
                x for x in data.get("items", [])
                if x.get("path") != path_or_id and x.get("id") != path_or_id
            ]
            if len(data["items"]) != before:
                write_json_atomic(target_file, data)
        return {"ok": True}

    def resolve_local_file(self, name: str, size: int = 0) -> str:
        """Resolves a filename + byte size to its exact absolute path on the user's disk."""
        if not name:
            return ""
        # 1. Check workspace root & src/ first
        for base in (self.root_dir / "src", self.root_dir / "Asset", self.root_dir):
            if base.is_dir():
                for f in base.rglob(name):
                    if f.is_file() and (size <= 0 or f.stat().st_size == size):
                        return norm_abs(f)

        # 2. Check common user media folders (Desktop, Downloads, Videos, Music, Pictures)
        home = Path.home()
        for folder_name in ("Videos", "Music", "Pictures", "Downloads", "Desktop", "Documents"):
            d = home / folder_name
            if d.is_dir():
                try:
                    for f in d.rglob(name):
                        if f.is_file() and (size <= 0 or f.stat().st_size == size):
                            return norm_abs(f)
                except Exception:
                    pass
        return ""

    def get_bootstrap_bundle(self) -> dict:
        ws = read_json_safe(self.workspace_file, {})
        active_path = ws.get("activeProjectPath") or ws.get("activeProjectName") or "Hotel Room"
        active_proj = self.read_project_payload(active_path)
        if not active_proj:
            active_proj = self.read_project_payload("Hotel Room")

        manifest_data = read_json_safe(self.root_dir / "Asset" / "manifest.json", {})
        media_items = read_json_safe(self.media_file, {"items": []}).get("items", [])
        audio_items = read_json_safe(self.audio_file, {"items": []}).get("items", [])
        text_items = read_json_safe(self.text_file, {"items": []}).get("items", [])
        return {
            "anchor": read_json_safe(self.anchor_file, {}),
            "settings": read_json_safe(self.settings_file, {}),
            "workspace": ws,
            "recent": read_json_safe(self.recent_file, {}),
            "projects": read_json_safe(self.projects_file, {"items": []}).get("items", []),
            "media": media_items,
            "mediaLinks": media_items,
            "audio": audio_items,
            "audioLinks": audio_items,
            "text": text_items,
            "textLinks": text_items,
            "fonts": read_json_safe(self.fonts_file, {"items": []}).get("items", []),
            "builtinManifest": manifest_data,
            "activeProject": active_proj,
        }


REGISTRY = StockCutDiskRegistry(ROOT, STOCKCUT_DIR)


class RangeFileWrapper:
    """Wraps a file object to stream only the requested byte range (Zero RAM preload)."""
    def __init__(self, file_obj, start, length):
        self.file_obj = file_obj
        self.file_obj.seek(start)
        self.remaining = length

    def read(self, size=-1):
        if self.remaining <= 0:
            return b""
        if size < 0 or size > self.remaining:
            size = self.remaining
        data = self.file_obj.read(size)
        self.remaining -= len(data)
        return data

    def close(self):
        self.file_obj.close()


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.mjs': 'text/javascript',
        '.json': 'application/json',
        '.ttf': 'font/ttf',
        '.otf': 'font/otf',
        '.woff': 'font/woff',
        '.woff2': 'font/woff2',
        '.mp3': 'audio/mpeg',
        '.m4a': 'audio/mp4',
        '.wav': 'audio/wav',
        '.ogg': 'audio/ogg',
        '.flac': 'audio/flac',
        '.aac': 'audio/aac',
        '.mp4': 'video/mp4',
        '.webm': 'video/webm',
        '.mov': 'video/quicktime',
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.png': 'image/png',
        '.webp': 'image/webp',
    }

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, Range')
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(http.HTTPStatus.NO_CONTENT)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _send_json(self, obj, status=http.HTTPStatus.OK):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> dict:
        length = int(self.headers.get('Content-Length', '0') or '0')
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode('utf-8'))

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        route = parsed.path

        if route == '/@stockcut/bootstrap':
            self._send_json(REGISTRY.get_bootstrap_bundle())
            return

        if route == '/@stockcut/project':
            qs = urllib.parse.parse_qs(parsed.query)
            target = (qs.get('path') or qs.get('name') or [''])[0]
            payload = REGISTRY.read_project_payload(target)
            if not payload:
                self._send_json({"ok": False, "error": f"Project not found: {target}"}, http.HTTPStatus.NOT_FOUND)
                return
            REGISTRY.update_workspace({
                "activeProjectName": payload["name"],
                "activeProjectPath": payload["rootPath"],
            })
            self._send_json({"ok": True, "project": payload})
            return

        super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlsplit(self.path)
        route = parsed.path
        try:
            body = self._read_json_body()
        except Exception as e:
            self._send_json({"ok": False, "error": str(e)}, http.HTTPStatus.BAD_REQUEST)
            return

        if route == '/@stockcut/project/save':
            res = REGISTRY.save_project_payload(body)
            self._send_json(res)
            return

        if route == '/@stockcut/workspace':
            ws = REGISTRY.update_workspace(body)
            self._send_json({"ok": True, "workspace": ws})
            return

        if route == '/@stockcut/links/add':
            items = body.get("items") if isinstance(body.get("items"), list) else [body]
            added = []
            for it in items:
                if isinstance(it, dict):
                    # Resolve real disk path if only filename + size is provided
                    p = it.get("path") or ""
                    if (not p or p.startswith("/Asset/")) and not it.get("isAsset"):
                        resolved = REGISTRY.resolve_local_file(it.get("name", ""), int(it.get("size", 0) or 0))
                        if resolved:
                            it["path"] = resolved
                            it["src"] = resolved
                    r = REGISTRY.add_link(it)
                    if r.get("ok"):
                        added.append(r["item"])
            self._send_json({"ok": True, "items": added})
            return

        if route == '/@stockcut/links/remove':
            target = body.get("path") or body.get("id") or ""
            self._send_json(REGISTRY.remove_link(target))
            return

        if route == '/@stockcut/links/resolve':
            resolved = REGISTRY.resolve_local_file(body.get("name", ""), int(body.get("size", 0) or 0))
            self._send_json({"ok": bool(resolved), "path": resolved})
            return

        self._send_json({"ok": False, "error": f"Unknown endpoint {route}"}, http.HTTPStatus.NOT_FOUND)

    def translate_path(self, path):
        path = path.split('?', 1)[0]
        path = path.split('#', 1)[0]
        unquoted = urllib.parse.unquote(path)

        if unquoted.startswith('/@fs/'):
            raw = unquoted[5:]
            if len(raw) > 2 and raw[1] == ':':
                return os.path.normpath(raw)
            if len(raw) > 3 and raw[0] == '/' and raw[2] == ':':
                return os.path.normpath(raw[1:])
            return os.path.normpath(raw)

        if len(unquoted) >= 3 and unquoted[0] == '/' and unquoted[2] == ':' and unquoted[1].isalpha():
            return os.path.normpath(unquoted[1:])

        return super().translate_path(path)

    def send_head(self):
        if self.path.split('?')[0] == '/favicon.ico':
            self.send_response(http.HTTPStatus.NO_CONTENT)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        path = self.translate_path(self.path)
        f = None
        if os.path.isdir(path):
            parts = urllib.parse.urlsplit(self.path)
            if not parts.path.endswith('/'):
                self.send_response(http.HTTPStatus.MOVED_PERMANENTLY)
                new_parts = (parts[0], parts[1], parts[2] + '/', parts[3], parts[4])
                new_url = urllib.parse.urlunsplit(new_parts)
                self.send_header("Location", new_url)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            for index in "index.html", "index.htm":
                index_path = os.path.join(path, index)
                if os.path.exists(index_path):
                    path = index_path
                    break
            else:
                return self.list_directory(path)

        ctype = self.guess_type(path)
        try:
            f = open(path, 'rb')
        except OSError:
            self.send_error(http.HTTPStatus.NOT_FOUND, "File not found")
            return None

        try:
            fs = os.fstat(f.fileno())
            total_size = fs.st_size
            etag = f'"{fs.st_mtime_ns:x}-{total_size:x}"'

            range_header = self.headers.get('Range')
            if not range_header and self.headers.get('If-None-Match') == etag:
                f.close()
                self.send_response(http.HTTPStatus.NOT_MODIFIED)
                self.send_header("ETag", etag)
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                return None

            if range_header and range_header.startswith('bytes='):
                try:
                    ranges = range_header[6:].split('-')
                    start = int(ranges[0]) if ranges[0] else 0
                    end = int(ranges[1]) if len(ranges) > 1 and ranges[1] else total_size - 1
                    if start >= total_size or end >= total_size or start > end:
                        self.send_error(http.HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                        f.close()
                        return None

                    length = end - start + 1
                    self.send_response(http.HTTPStatus.PARTIAL_CONTENT)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Range", f"bytes {start}-{end}/{total_size}")
                    self.send_header("Content-Length", str(length))
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("ETag", etag)
                    self.send_header("Cache-Control", "no-cache")
                    self.end_headers()
                    return RangeFileWrapper(f, start, length)
                except Exception:
                    pass

            self.send_response(http.HTTPStatus.OK)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(total_size))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", etag)
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            return f
        except Exception:
            f.close()
            raise

    def copyfile(self, source, outputfile):
        try:
            super().copyfile(source, outputfile)
        except (ConnectionResetError, BrokenPipeError):
            pass

    def log_message(self, fmt, *args):
        msg = fmt % args
        if '404' in msg or '500' in msg:
            super().log_message(fmt, *args)


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == '__main__':
    handler = partial(IsolatedHandler, directory=str(ROOT))
    with ThreadingServer(('127.0.0.1', PORT), handler) as httpd:
        print(f'[serve] http://127.0.0.1:{PORT}/index.html  (COOP/COEP enabled, .stockcut={STOCKCUT_DIR})')
        httpd.serve_forever()
