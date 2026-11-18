import os
import sys
import subprocess
import shutil
import re
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

NATIVE_DIR = os.path.dirname(os.path.abspath(__file__))
EMSDK_ENV_BAT = r"C:\emsdk\emsdk_env.bat"

SRCS = [
    'core/memory.cpp',
    'core/templates.cpp',
    'core/cut_project.cpp',
    'core/math_utils.cpp',
    'cache/transposition_table.cpp',
    'cache/zobrist.cpp',
    'spatial/dirty_rect.cpp',
    'timeline/transfer.cpp',
    'timeline/effect.cpp',
    'render/renderer.cpp',
    'render/graph.cpp',
    'render/subpass.cpp',
    'audio/dsp.cpp',
    'audio/script.cpp',
    'core/telemetry.cpp',
    'core/display_list.cpp',
    'core/style.cpp',
    'text/layout.cpp',
    'core/geometry.cpp',
    'core/filter.cpp',
    'core/pipeline.cpp',
    'timeline/position/timeline_position.cpp',
    'timeline/position/display_list.cpp',
    'timeline/position/wasm_exports.cpp',
    'timeline/runtime/runtime.cpp',
    'text/reveal.cpp',
    'text/font_engine.cpp',
    'text/font_map.cpp',
    'core/tracker.cpp',
    'vbo/batcher.cpp',
    'core/shared_state.cpp',
    'effect/engine.cpp',
    'effect/fx_math.cpp',
    'effect/raindrop_suite_fx.cpp',
    'effect/fusion_kernel.cpp',
    'cache/computation_cache.cpp',
    'core/obs_matrix.cpp',
    'spatial/roi.cpp',
    'core/scale.cpp',
    'core/manager.cpp',
    'core/nnue_project.cpp',
    'core/config.cpp',
    'core/undo.cpp',
    'core/stream.cpp',
    'core/registry.cpp',
    'timeline/project/json_reader.cpp',
    'timeline/project/compiler.cpp',
    'timeline/project/duration.cpp',
    'timeline/project/json_buffer.cpp',
    'core/wasm_entry.cpp',
    'core/engine.cpp'
]

CXXFLAGS = [
    '-O3',
    '-std=c++20',
    '-msimd128',
    '-mbulk-memory',
    '-fno-exceptions',
    '-fno-rtti',
    '-sUSE_FREETYPE=1',
    '-sUSE_HARFBUZZ=1',
    '-I.',
    '-Icore',
    '-Icache',
    '-Ispatial',
    '-Ieffect',
    '-Itext',
    '-Iaudio',
    '-Ivbo',
    '-Irender',
    '-Iquantum',
    '-Itimeline',
    '-Itimeline/position',
    '-Itimeline/project',
    '-Itimeline/runtime',
    '-Iagent',
    '-I../LLM/native',
    '-I.build/woff2/_deps/woff2_source-src/include',
    '-I.build/woff2/_deps/woff2_source-src/src',
    '-I.build/woff2/_deps/brotli_source-src/c/include',
    '-Iwoff2/vendor/woff2/include',
    '-Iwoff2/vendor/brotli/c/include'
]

LDFLAGS = [
    '-Wl,--no-entry',
    '-Wl,--export-all',
    '-sUSE_FREETYPE=1',
    '-sUSE_HARFBUZZ=1',
    '-sUSE_WEBGL2=1',
    '-sFULL_ES3=1',
    '-sMIN_WEBGL_VERSION=2',
    '-sMAX_WEBGL_VERSION=2',
    '-sMODULARIZE=1',
    '-sEXPORT_NAME=createStockcutModule',
    '-sEXPORT_ALL=1',
    '-sINITIAL_MEMORY=134217728',
    '-sMAXIMUM_MEMORY=1073741824',
    '-sSTACK_SIZE=4194304',
    '-sALLOW_MEMORY_GROWTH=1',
    '-sDISABLE_EXCEPTION_THROWING=0',
    '-fexceptions',
    '-sWASM_BIGINT=1'
]

LDLIBS = [
    '-L.build/woff2',
    '-lwoff2dec',
    '-lwoff2common',
    '-lbrotlidec',
    '-lbrotlicommon'
]

env = os.environ.copy()
env['PATH'] = r'C:\emsdk;C:\emsdk\upstream\emscripten;C:\emsdk\node\24.19.0_64bit;' + env.get('PATH', '')
env['EMSDK'] = 'C:/emsdk'
env['EMSDK_NODE'] = r'C:\emsdk\node\24.19.0_64bit\node.exe'
env['EMSDK_PYTHON'] = r'C:\emsdk\python\3.13.3_64bit\python.exe'

EMXX = r'C:\emsdk\upstream\emscripten\em++.exe'

def object_is_current(obj: Path, depfile: Path) -> bool:
    try:
        rule = depfile.read_text(encoding='utf-8').replace('\\\n', ' ').splitlines()[0]
        names = re.findall(r'(?:\\ |[^\s])+', rule.split(': ', 1)[1])
        stamp = obj.stat().st_mtime_ns
        return bool(names) and stamp > Path(__file__).stat().st_mtime_ns and all(
            stamp > (Path(NATIVE_DIR) / name.replace(r'\ ', ' ')).stat().st_mtime_ns
            for name in names
        )
    except (OSError, IndexError):
        return False

def compile_src(src: str):
    obj = src.rsplit('.', 1)[0] + '.o'
    obj_path = Path(NATIVE_DIR) / obj
    depfile = obj_path.with_suffix('.d')
    if object_is_current(obj_path, depfile):
        return obj, True
    cmd = [EMXX] + CXXFLAGS
    if src in ('timeline/project/json_reader.cpp', 'timeline/project/duration.cpp'):
        cmd.append('-fno-builtin')
    cmd += ['-MMD', '-MP', '-MF', str(depfile), '-c', src, '-o', obj]
    res = subprocess.run(cmd, cwd=NATIVE_DIR, env=env, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAILED {src}:")
        print(res.stderr)
        return obj, False
    print(f"Compiled {src}")
    return obj, True

def backup_previous_wasm():
    backup_dir = os.path.join(NATIVE_DIR, 'previous')
    wasm_path = os.path.join(NATIVE_DIR, 'stockcut.wasm')
    if not os.path.exists(wasm_path):
        wasm_js_path = os.path.join(NATIVE_DIR, '..', 'js', 'stockcut.wasm')
        if os.path.exists(wasm_js_path):
            wasm_path = wasm_js_path
    if os.path.exists(wasm_path):
        os.makedirs(backup_dir, exist_ok=True)
        backup_wasm = os.path.join(backup_dir, 'stockcut.wasm')
        shutil.copy2(wasm_path, backup_wasm)
        js_path = os.path.join(NATIVE_DIR, 'stockcut.js')
        if os.path.exists(js_path):
            shutil.copy2(js_path, os.path.join(backup_dir, 'stockcut.js'))
        runtime_path = os.path.join(NATIVE_DIR, '..', 'js', 'stockcut_runtime.js')
        if os.path.exists(runtime_path):
            shutil.copy2(runtime_path, os.path.join(backup_dir, 'stockcut_runtime.js'))
        size_bytes = os.path.getsize(backup_wasm)
        import datetime
        import json
        meta = {
            'timestamp': datetime.datetime.now().isoformat(),
            'size_bytes': size_bytes,
            'source': wasm_path
        }
        with open(os.path.join(backup_dir, 'build_meta.json'), 'w', encoding='utf-8') as f:
            json.dump(meta, f, indent=2)
        print(f"[PRE-BUILD BACKUP] Archived prior WASM ({size_bytes:,} bytes) to native/previous/stockcut.wasm")
    else:
        print("[PRE-BUILD BACKUP] No existing WASM found to archive.")

def main():
    backup_previous_wasm()
    print("Compiling modules...")
    objs = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as executor:
        results = list(executor.map(compile_src, SRCS))
    for obj, ok in results:
        if not ok:
            print("Compilation error, aborting.")
            sys.exit(1)
        objs.append(obj)
    print("Linking stockcut.js...")
    link_cmd = [EMXX] + CXXFLAGS + LDFLAGS + ['-o', 'stockcut.js'] + objs + LDLIBS
    res = subprocess.run(link_cmd, cwd=NATIVE_DIR, env=env, capture_output=True, text=True)
    if res.returncode != 0:
        print("LINKING FAILED:")
        print(res.stderr)
        sys.exit(1)
    print("Linking complete!")
    wasm_src = os.path.join(NATIVE_DIR, 'stockcut.wasm')
    js_src = os.path.join(NATIVE_DIR, 'stockcut.js')
    wasm_dst = os.path.join(NATIVE_DIR, '..', 'js', 'stockcut.wasm')
    js_dst = os.path.join(NATIVE_DIR, '..', 'js', 'stockcut_runtime.js')
    shutil.copy2(wasm_src, wasm_dst)
    shutil.copy2(js_src, js_dst)
    print(f"Copied artifacts to js/ (wasm size: {os.path.getsize(wasm_src)} bytes)")
    print("Running update_b64.js...")
    node_exe = env.get('EMSDK_NODE', 'node')
    res = subprocess.run([node_exe, 'update_b64.js'], cwd=NATIVE_DIR, env=env, capture_output=True, text=True)
    if res.returncode != 0:
        print("update_b64.js failed:")
        print(res.stderr)
        sys.exit(1)
    print(res.stdout)
    print("BUILD SUCCESSFUL!")

if __name__ == '__main__':
    main()
