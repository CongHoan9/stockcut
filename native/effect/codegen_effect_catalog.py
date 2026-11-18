#!/usr/bin/env python3
"""Zero-Enum Self-Contained Binary Blob & Metaprogramming Codegen.

Reads ExtractedEffects/effects_catalog.json (170) and fills_catalog.json (45)
and generates:
1. native/effect/catalog.h:
   - ZERO enums, ZERO symbolic schema/param structs.
   - Single self-contained raw binary blob: alignas(64) constexpr u8 kCatalogBlob[].
   - Compile-time constexpr metaprogramming & pointer-arithmetic policy functions.
2. BIT_AND_REGISTER_DICTIONARY.txt:
   - Complete human-readable debug dictionary mapping every bitfield, register
     offset, hash, and binary blob offset for all MediaType Blocks and all 215
     Effects (2,226 parameters).
"""

import json
import math
import struct
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent.parent
EFFECTS_JSON = ROOT_DIR / "ExtractedEffects" / "effects_catalog.json"
FILLS_JSON = ROOT_DIR / "ExtractedEffects" / "fills_catalog.json"
OUT_HEADER = Path(__file__).resolve().parent / "catalog.h"
OUT_DICT_TXT = ROOT_DIR / "BIT_AND_REGISTER_DICTIONARY.txt"


# ============================================================================
# 64-BIT ANONYMOUS ALGEBRA WORD CONSTANTS (ZERO NAMED-EFFECT ENUMS)
# Must stay bit-identical to native/effect/algebra.h
# ============================================================================
ABI_UV     = 1 << 0
ABI_SAMPLE = 1 << 1
ABI_COLOR  = 1 << 2
ABI_GEOM   = 1 << 3

DOM_D0_FULL       = 1 << 4
DOM_D1_BOUNDED    = 1 << 5
DOM_D2_HALO       = 1 << 6
DOM_D3_SPARSE     = 1 << 7
DOM_D4_MULTISCALE = 1 << 8
DOM_D5_TEMPORAL   = 1 << 9
DOM_D6_AUDIO_SYNC = 1 << 10

TRAIT_AFFINE_2D     = 1 << 11
TRAIT_AFFINE_COLOR  = 1 << 12
TRAIT_GAUSSIAN_CONV = 1 << 13
TRAIT_IDENTITY_GATE = 1 << 14
TRAIT_COMMUTATIVE   = 1 << 15

AUX_NOISE_ATLAS  = 1 << 16
AUX_MIP_PYRAMID  = 1 << 17
AUX_TILE_LUMA    = 1 << 18
AUX_EDGE_SOBEL   = 1 << 19
AUX_ALPHA_SDF    = 1 << 20
AUX_AUDIO_FFT    = 1 << 21
AUX_DEPTH_MAP    = 1 << 22
AUX_HISTORY_RING = 1 << 23


def classify_algebra_word(
    name: str,
    res_path: str,
    script_class: str,
    shader_str: str,
    is_fill: int,
    local_params: list,
) -> int:
    key = f"{name} {res_path} {script_class} {shader_str}".lower()
    abi = 0
    dom = 0
    traits = 0
    aux = 0
    shape_class = 0
    alu_weight = 16

    # 1. Determine 4-Interface I/O ABI & 7-Domain Support
    if "audio" in key or "spectrum" in key or "sound" in key:
        abi = ABI_COLOR
        dom = DOM_D6_AUDIO_SYNC | DOM_D1_BOUNDED
        aux |= AUX_AUDIO_FFT
        alu_weight = 24
    elif "particle" in key or "lensflare" in key or "raindrop" in key or "3d" in key or "saber" in key or "layerstyles" in key:
        abi = ABI_GEOM
        dom = DOM_D0_FULL | DOM_D4_MULTISCALE
        if "raindrop" in key or "particle" in key:
            dom |= DOM_D5_TEMPORAL
            aux |= AUX_NOISE_ATLAS
        if "layerstyles" in key or "sdf" in key:
            aux |= AUX_ALPHA_SDF
        alu_weight = 96
    elif any(k in key for k in ("bloom", "glow", "lightleak", "godray", "rays", "glint", "flare")):
        abi = ABI_SAMPLE | ABI_COLOR
        dom = DOM_D3_SPARSE | DOM_D4_MULTISCALE | DOM_D2_HALO
        aux |= AUX_MIP_PYRAMID | AUX_TILE_LUMA
        alu_weight = 48
    elif any(k in key for k in ("blur", "bokeh", "sharpen", "minimax", "median", "denoise", "bevel", "emboss", "edge", "sobel", "fxaa", "outline")):
        abi = ABI_SAMPLE
        dom = DOM_D2_HALO
        if "gaussian" in key:
            traits |= TRAIT_GAUSSIAN_CONV | TRAIT_COMMUTATIVE
            dom |= DOM_D4_MULTISCALE
            aux |= AUX_MIP_PYRAMID
        if any(k in key for k in ("edge", "sobel", "outline", "sharpen")):
            aux |= AUX_EDGE_SOBEL
        if "spot" in key or "tilt" in key or "radial" in key or "zoom" in key:
            dom |= DOM_D1_BOUNDED
            shape_class = 1
        alu_weight = 40
    elif any(k in key for k in ("twirl", "pinch", "bulge", "lens", "fisheye", "spherize", "swirl", "magnify")):
        abi = ABI_UV
        dom = DOM_D1_BOUNDED
        shape_class = 1  # RadialDisk
        alu_weight = 14
    elif any(k in key for k in ("glitch", "slice", "shift", "scan", "vhs", "jitter", "mosaic", "pixel", "tile", "mirror", "kaleido", "flip", "transform", "shake", "zoom", "pan", "bend", "wave", "ripple", "turbulent", "displace", "distort", "warp", "liquify", "jiggle", "chromatic", "rgb")):
        if any(k in key for k in ("chromatic", "rgb", "glitch2", "vhs")):
            abi = ABI_SAMPLE
            dom = DOM_D2_HALO
            alu_weight = 32
        else:
            abi = ABI_UV
            if any(k in key for k in ("glitch", "slice", "shift")):
                dom = DOM_D1_BOUNDED
                shape_class = 2  # ScanStrip+Block
            else:
                dom = DOM_D0_FULL
            if any(k in key for k in ("transform", "mirror", "flip", "zoom", "shake", "pan")):
                traits |= TRAIT_AFFINE_2D
            if any(k in key for k in ("turbulent", "noise", "ripple", "displace", "jiggle", "wave")):
                aux |= AUX_NOISE_ATLAS
            alu_weight = 16
    elif any(k in key for k in ("echo", "trail", "feedback", "time", "ghost", "frame")):
        abi = ABI_SAMPLE | ABI_COLOR
        dom = DOM_D5_TEMPORAL | DOM_D0_FULL
        aux |= AUX_HISTORY_RING
        alu_weight = 36
    elif is_fill == 1 or any(k in key for k in ("noise", "fractal", "fire", "flame", "smoke", "cloud", "rain", "snow", "star", "matrix", "grid", "gradient", "checker", "stripe", "plasma", "burn")):
        abi = ABI_COLOR
        dom = DOM_D0_FULL
        if "border" in key or "frame" in key or "vignette" in key:
            dom = DOM_D1_BOUNDED
            shape_class = 3  # BorderFrame
        if any(k in key for k in ("noise", "fractal", "fire", "flame", "smoke", "cloud", "burn", "plasma")):
            aux |= AUX_NOISE_ATLAS
        alu_weight = 28
    else:
        # Pure Color / Grading / LUT / Threshold / Posterize / Tint / Brightness / Exposure / Contrast
        abi = ABI_COLOR
        dom = DOM_D0_FULL
        if any(k in key for k in ("bright", "exposure", "tint", "fade", "gain", "invert", "gamma", "color", "flicker", "fill", "opacity")):
            traits |= TRAIT_AFFINE_COLOR
            if any(k in key for k in ("bright", "exposure", "tint", "fade", "gain")):
                traits |= TRAIT_COMMUTATIVE
        alu_weight = 10

    if "depth" in key or "fog" in key or "dof" in key:
        aux |= AUX_DEPTH_MAP

    # 2. Scan parameters for identity gate register & bounded radius/size register
    bounded_reg = 0xFF
    gate_reg = 0xFF
    for p in local_params:
        if (p["trait_flags"] & 0x01) != 0:
            continue  # skip bit layer
        pn = p["name"].lower()
        if bounded_reg == 0xFF and any(k in pn for k in ("radius", "size", "width", "range", "distance", "blur")):
            bounded_reg = p["offset"] & 0xFF
        if gate_reg == 0xFF and any(k in pn for k in ("strength", "amount", "intensity", "mix", "alpha", "opacity", "weight", "power", "scale")):
            gate_reg = p["offset"] & 0xFF
            traits |= TRAIT_IDENTITY_GATE

    word = (
        (abi & 0xF)
        | (dom & (0x7F << 4))
        | (traits & (0x1F << 11))
        | (aux & (0xFF << 16))
        | ((bounded_reg & 0xFF) << 24)
        | ((gate_reg & 0xFF) << 32)
        | ((alu_weight & 0xFF) << 40)
        | ((shape_class & 0xFF) << 48)
    )
    return word & 0xFFFFFFFFFFFFFFFF


def fnv1a_32(text: str) -> int:
    h = 2166136261
    for b in text.strip().lower().encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def classify_exec_mode(script_class: str, shader: str) -> int:
    sc = script_class or ""
    if sc in ("NodeVideo.AE.AESinglePass", "NodeVideo.AE.AEVHS", "NodeVideo.AE.AEDissolve"):
        return 0  # Single-pass shader
    if sc == "NodeVideo.AE.AELensFlare" or "Particle" in sc:
        return 2  # Prefab particle
    if "Audio" in sc:
        return 3  # Audio visualizer
    if "AECPU" in sc:
        return 4  # CPU worker kernel
    if shader and sc.startswith("NodeVideo.AE."):
        return 1  # Multi-pass graph
    return 1


def pack_effect_header_word(
    float_count: int,
    param_count: int,
    tier_shift: int,
    exec_mode: int,
    bit_words: int,
    is_fill: int,
    tex_count: int,
    cube_count: int,
    shader_slot: int,
    default_floats_rel_off: int,
) -> int:
    assert 0 <= float_count <= 255
    assert 0 <= param_count <= 255
    assert 0 <= tier_shift <= 3
    assert 0 <= exec_mode <= 7
    assert 0 <= bit_words <= 3
    assert 0 <= is_fill <= 1
    assert 0 <= tex_count <= 15
    assert 0 <= cube_count <= 15
    assert 0 <= shader_slot <= 0xFFFF
    assert 0 <= default_floats_rel_off <= 0xFFFF

    word = (
        (float_count & 0xFF)
        | ((param_count & 0xFF) << 8)
        | ((tier_shift & 0x3) << 16)
        | ((exec_mode & 0x7) << 18)
        | ((bit_words & 0x3) << 21)
        | ((is_fill & 0x1) << 23)
        | ((tex_count & 0xF) << 24)
        | ((cube_count & 0xF) << 28)
        | ((shader_slot & 0xFFFF) << 32)
        | ((default_floats_rel_off & 0xFFFF) << 48)
    )
    return word & 0xFFFFFFFFFFFFFFFF


def pack_param_spec_word(
    trait_flags: int,
    offset: int,
    width: int,
    dep_bool_bit: int,
    dep_enum_bit_off: int,
    dep_enum_bit_width: int,
    enum_allowed_mask: int,
) -> int:
    assert 0 <= trait_flags <= 0xFF
    assert 0 <= offset <= 0xFF
    assert 0 <= width <= 0x3F
    assert 0 <= dep_bool_bit <= 0xFF
    assert 0 <= dep_enum_bit_off <= 0xFF
    assert 0 <= dep_enum_bit_width <= 0xF
    assert 0 <= enum_allowed_mask <= 0x3FFFFF

    word = (
        (trait_flags & 0xFF)
        | ((offset & 0xFF) << 8)
        | ((width & 0x3F) << 16)
        | ((dep_bool_bit & 0xFF) << 22)
        | ((dep_enum_bit_off & 0xFF) << 30)
        | ((dep_enum_bit_width & 0xF) << 38)
        | ((enum_allowed_mask & 0x3FFFFF) << 42)
    )
    return word & 0xFFFFFFFFFFFFFFFF


def main() -> None:
    effects_raw = json.loads(EFFECTS_JSON.read_text(encoding="utf-8"))
    fills_raw = json.loads(FILLS_JSON.read_text(encoding="utf-8"))

    all_entries = []
    for item in effects_raw:
        all_entries.append((item, 0))
    for item in fills_raw:
        all_entries.append((item, 1))

    shader_names = {}
    shader_list = []

    # Blob layout constants:
    # [0 .. 4095]   : 512-slot open-addressed Hash Directory (512 * 8B: u32 hash, u32 recordByteOff)
    # [4096 .. 4959]: 216-slot u32 Ordinal Effect Offset Table (215 effects + 1 total size sentinel, 16B aligned)
    # [4960 .. end] : 215 Self-Contained 16B-aligned Binary Effect Records
    HASH_DIR_CAP = 512
    HASH_DIR_BYTES = HASH_DIR_CAP * 8  # 4096 bytes
    ORDINAL_TABLE_OFFSET = HASH_DIR_BYTES
    ORDINAL_TABLE_ENTRIES = 216  # 215 + 1 sentinel -> 864 bytes (4096 + 864 = 4960, divisible by 16!)
    RECORDS_START_OFFSET = ORDINAL_TABLE_OFFSET + ORDINAL_TABLE_ENTRIES * 4  # 4960

    blob = bytearray(RECORDS_START_OFFSET)
    effect_records_meta = []

    for effect_idx, (entry, is_fill) in enumerate(all_entries):
        name = str(entry.get("name") or entry.get("m_Name") or "")
        res_path = str(entry.get("resourcePath") or "").strip().lower()
        script_class = str(entry.get("scriptClass") or "")
        shader_str = str(entry.get("shader") or "")
        textures = entry.get("textures") or []
        cubemaps = entry.get("cubemaps") or []
        params = entry.get("params") or []

        if shader_str:
            if shader_str not in shader_names:
                shader_names[shader_str] = len(shader_list)
                shader_list.append(shader_str)
            shader_slot = shader_names[shader_str]
        else:
            shader_slot = 0xFFFF

        exec_mode = classify_exec_mode(script_class, shader_str)
        param_count = len(params)

        bit_cursor = 0
        float_cursor = 0
        default_bits0 = 0
        default_floats = []
        group_enable_bit = {}
        local_params = []

        for local_idx, p in enumerate(params):
            pname = str(p.get("name") or "")
            ptype = str(p.get("type") or "Float")
            pgroup = str(p.get("group") or "")
            raw_def = p.get("default")
            clamp = bool(p.get("clamp", False))
            above_zero = bool(p.get("aboveZero", False))
            group_enabler = bool(p.get("groupEnableByMe", False))
            cycle = float(p.get("cycle") or 0.0)
            enums = p.get("enums") or []
            raw_min = p.get("min")
            raw_max = p.get("max")

            min_val = 0.0
            max_val = 1.0
            has_min = False
            has_max = False
            if isinstance(raw_min, (int, float)):
                min_val = float(raw_min)
                has_min = True
            elif isinstance(raw_min, list) and len(raw_min) > 0:
                min_val = float(raw_min[0])
                has_min = True

            if isinstance(raw_max, (int, float)):
                max_val = float(raw_max)
                has_max = True
            elif isinstance(raw_max, list) and len(raw_max) > 0:
                max_val = float(raw_max[0])
                has_max = True

            # Trait bitflags (Bits 0..7 of specWord):
            # bit 0 (0x01): IS_BIT_LAYER
            # bit 1 (0x02): IS_RAW_U32
            # bit 2 (0x04): ROUND_INT
            # bit 3 (0x08): CLAMP
            # bit 4 (0x10): ABOVE_ZERO
            # bit 5 (0x20): HAS_CYCLE
            # bit 6 (0x40): HAS_MIN
            # bit 7 (0x80): HAS_MAX
            trait_flags = 0
            if clamp:
                trait_flags |= 0x08
            if above_zero:
                trait_flags |= 0x10
            if cycle > 0.0 and has_min and has_max and not clamp:
                trait_flags |= 0x20
            if has_min:
                trait_flags |= 0x40
            if has_max:
                trait_flags |= 0x80

            if ptype == "Bool":
                trait_flags |= 0x01  # IS_BIT_LAYER
                offset = bit_cursor
                width = 1
                bit_cursor += 1
                bval = bool(raw_def) if raw_def is not None else False
                if bval:
                    default_bits0 |= (1 << offset)
                if group_enabler and pgroup:
                    group_enable_bit[pgroup] = offset

            elif ptype == "Enum":
                trait_flags |= 0x01 | 0x04  # IS_BIT_LAYER | ROUND_INT
                enum_cnt = len(enums)
                if enum_cnt > 0:
                    max_val = float(enum_cnt - 1)
                    min_val = 0.0
                    trait_flags |= 0x40 | 0x80  # HAS_MIN | HAS_MAX
                elif has_max:
                    enum_cnt = int(max_val) + 1
                else:
                    enum_cnt = 2
                bit_width = max(1, math.ceil(math.log2(max(2, enum_cnt))))
                offset = bit_cursor
                width = bit_width
                bit_cursor += bit_width
                eval_int = int(raw_def) if isinstance(raw_def, (int, float)) else 0
                eval_int = max(0, min(eval_int, (1 << bit_width) - 1))
                default_bits0 |= (eval_int << offset)

            elif ptype == "Int":
                trait_flags |= 0x04  # ROUND_INT
                offset = float_cursor
                width = 1
                float_cursor += 1
                ival = int(round(float(raw_def))) if isinstance(raw_def, (int, float)) else 0
                default_floats.append(float(ival))

            elif ptype == "Float":
                offset = float_cursor
                width = 1
                float_cursor += 1
                fval = float(raw_def) if isinstance(raw_def, (int, float)) else 0.0
                default_floats.append(fval)

            elif ptype == "Vector2":
                offset = float_cursor
                width = 2
                float_cursor += 2
                if isinstance(raw_def, list) and len(raw_def) >= 2:
                    default_floats.extend([float(raw_def[0]), float(raw_def[1])])
                else:
                    default_floats.extend([0.0, 0.0])

            elif ptype == "Vector3":
                offset = float_cursor
                width = 3
                float_cursor += 3
                if isinstance(raw_def, list) and len(raw_def) >= 3:
                    default_floats.extend([float(raw_def[0]), float(raw_def[1]), float(raw_def[2])])
                else:
                    default_floats.extend([0.0, 0.0, 0.0])

            elif ptype == "Color":
                offset = float_cursor
                width = 4
                float_cursor += 4
                if isinstance(raw_def, list) and len(raw_def) >= 4:
                    default_floats.extend([float(raw_def[0]), float(raw_def[1]), float(raw_def[2]), float(raw_def[3])])
                else:
                    default_floats.extend([1.0, 1.0, 1.0, 1.0])

            elif ptype == "TexTransform":
                offset = float_cursor
                width = 4
                float_cursor += 4
                if isinstance(raw_def, dict):
                    tiling = raw_def.get("tiling") or [1.0, 1.0]
                    offs = raw_def.get("offset") or [0.0, 0.0]
                    default_floats.extend([float(tiling[0]), float(tiling[1]), float(offs[0]), float(offs[1])])
                else:
                    default_floats.extend([1.0, 1.0, 0.0, 0.0])

            else:
                # Resource reference (TexPicker, MotionBlur, Path, Text, etc.) -> bit_cast<float>(u32)
                trait_flags |= 0x02  # IS_RAW_U32
                offset = float_cursor
                width = 1
                float_cursor += 1
                default_floats.append(0.0)

            local_params.append({
                "local_idx": local_idx,
                "name": pname,
                "group": pgroup,
                "ptype": ptype,
                "trait_flags": trait_flags,
                "offset": offset,
                "width": width,
                "min_val": min_val,
                "max_val": max_val,
                "group_enabler": group_enabler,
                "vis_enum_name": str(p.get("visibilityDependEnumName") or ""),
                "vis_enum_idxs": [int(i) for i in (p.get("visibilityDependEnumIndexs") or [])],
                "enums": enums,
            })

        aligned_floats = max(4, ((float_cursor + 3) // 4) * 4)
        while float_cursor < aligned_floats:
            default_floats.append(0.0)
            float_cursor += 1

        if aligned_floats <= 16:
            tier_shift = 0
        elif aligned_floats <= 32:
            tier_shift = 1
        elif aligned_floats <= 64:
            tier_shift = 2
        else:
            tier_shift = 3

        bit_words = 1 if bit_cursor <= 64 else 2

        # Resolve visibility dependencies directly to bit offsets (NO param index indirection!)
        for rec in local_params:
            pgroup = rec["group"]
            pname = rec["name"]
            dep_bool_bit = 0xFF
            if pgroup and pgroup in group_enable_bit and not rec["group_enabler"]:
                dep_bool_bit = group_enable_bit[pgroup]

            dep_enum_bit_off = 0xFF
            dep_enum_bit_width = 0
            enum_allowed_mask = 0x3FFFFF
            vis_enum_name = rec["vis_enum_name"]
            vis_enum_idxs = rec["vis_enum_idxs"]
            if vis_enum_name and vis_enum_idxs:
                found_cand = None
                for cand in local_params:
                    if cand["group"] == pgroup and cand["name"] == vis_enum_name and (cand["trait_flags"] & 0x01):
                        found_cand = cand
                        break
                if found_cand is None:
                    for cand in local_params:
                        full_cand = f"{cand['group']}/{cand['name']}" if cand["group"] else cand["name"]
                        if (cand["name"] == vis_enum_name or full_cand == vis_enum_name) and (cand["trait_flags"] & 0x01):
                            found_cand = cand
                            break
                if found_cand is not None:
                    dep_enum_bit_off = found_cand["offset"]
                    dep_enum_bit_width = found_cand["width"]
                    mask = 0
                    for idx_val in vis_enum_idxs:
                        if 0 <= idx_val < 22:
                            mask |= (1 << idx_val)
                    enum_allowed_mask = mask

            full_path = f"{pgroup}/{pname}" if pgroup else pname
            full_hash = fnv1a_32(full_path)
            bare_hash = fnv1a_32(pname)
            spec_word = pack_param_spec_word(
                rec["trait_flags"],
                rec["offset"],
                rec["width"],
                dep_bool_bit,
                dep_enum_bit_off,
                dep_enum_bit_width,
                enum_allowed_mask,
            )
            rec["full_path"] = full_path
            rec["full_hash"] = full_hash
            rec["bare_hash"] = bare_hash
            rec["dep_bool_bit"] = dep_bool_bit
            rec["dep_enum_bit_off"] = dep_enum_bit_off
            rec["dep_enum_bit_width"] = dep_enum_bit_width
            rec["enum_allowed_mask"] = enum_allowed_mask
            rec["spec_word"] = spec_word

        # Align current blob end to 16 bytes for this Effect Record
        while len(blob) % 16 != 0:
            blob.append(0)
        record_start_off = len(blob)

        # Layout inside Effect Record (24B header + 24B*N params + aligned default floats):
        # +0..7   : u64 headerWord
        # +8..15  : u64 defaultBits0
        # +16..23 : u64 algebraWord (4-bit ABI | 7-bit Domain | 5-bit Traits | 8-bit Aux | RegAddrs | ALU | Shape)
        # +24..   : param_count * 24B parameter records
        # +aligned: aligned_floats * 4B default floats (16B-aligned relative to blob & record)
        algebra_word = classify_algebra_word(
            name, res_path, script_class, shader_str, is_fill, local_params
        )
        raw_params_end_rel = 24 + param_count * 24
        default_floats_rel_off = (raw_params_end_rel + 15) & ~15

        header_word = pack_effect_header_word(
            float_count=aligned_floats,
            param_count=param_count,
            tier_shift=tier_shift,
            exec_mode=exec_mode,
            bit_words=bit_words,
            is_fill=is_fill,
            tex_count=min(6, len(textures)),
            cube_count=min(6, len(cubemaps)),
            shader_slot=shader_slot,
            default_floats_rel_off=default_floats_rel_off,
        )

        # Write ordinal offset entry
        struct.pack_into("<I", blob, ORDINAL_TABLE_OFFSET + effect_idx * 4, record_start_off)

        # Append headerWord, defaultBits0, and algebraWord (24 bytes)
        blob.extend(struct.pack("<QQQ", header_word, default_bits0, algebra_word))

        # Append each 24-byte Parameter Record: <IIQff
        for rec in local_params:
            blob.extend(
                struct.pack(
                    "<IIQff",
                    rec["full_hash"],
                    rec["bare_hash"],
                    rec["spec_word"],
                    float(rec["min_val"]),
                    float(rec["max_val"]),
                )
            )

        # Pad to default_floats_rel_off
        while (len(blob) - record_start_off) < default_floats_rel_off:
            blob.append(0)

        # Append default floats
        for fval in default_floats:
            if math.isnan(fval) or math.isinf(fval):
                fval = 0.0
            blob.extend(struct.pack("<f", fval))

        path_hash = fnv1a_32(res_path)
        name_hash = fnv1a_32(name)

        effect_records_meta.append({
            "effect_idx": effect_idx,
            "name": name,
            "res_path": res_path,
            "script_class": script_class,
            "shader": shader_str,
            "shader_slot": shader_slot,
            "is_fill": is_fill,
            "record_off": record_start_off,
            "header_word": header_word,
            "default_bits0": default_bits0,
            "algebra_word": algebra_word,
            "float_count": aligned_floats,
            "param_count": param_count,
            "tier_shift": tier_shift,
            "exec_mode": exec_mode,
            "bit_words": bit_words,
            "default_floats_rel_off": default_floats_rel_off,
            "default_floats": default_floats,
            "path_hash": path_hash,
            "name_hash": name_hash,
            "params": local_params,
        })

    # Pad final blob to 64-byte cacheline boundary
    while len(blob) % 64 != 0:
        blob.append(0)
    struct.pack_into("<I", blob, ORDINAL_TABLE_OFFSET + 215 * 4, len(blob))

    # Populate 512-slot open-addressed Hash Directory at blob[0..4095]
    # Priority 1: all 215 path_hash entries (unique per resourcePath)
    # Priority 2: name_hash entries (preferring _assetstore when duplicate names exist)
    hash_dir = [(0, 0)] * HASH_DIR_CAP

    def insert_hash(h: int, byte_off: int, overwrite: bool = False) -> None:
        if h == 0:
            return
        slot = h & (HASH_DIR_CAP - 1)
        for _ in range(HASH_DIR_CAP):
            cur_h, _ = hash_dir[slot]
            if cur_h == 0:
                hash_dir[slot] = (h, byte_off)
                return
            if cur_h == h:
                if overwrite:
                    hash_dir[slot] = (h, byte_off)
                return
            slot = (slot + 1) & (HASH_DIR_CAP - 1)
        raise RuntimeError("Hash directory full")

    for em in effect_records_meta:
        insert_hash(em["path_hash"], em["record_off"], overwrite=True)

    for em in effect_records_meta:
        is_assetstore = "/_assetstore/" in em["res_path"]
        insert_hash(em["name_hash"], em["record_off"], overwrite=is_assetstore)

    for slot_idx, (h_val, off_val) in enumerate(hash_dir):
        struct.pack_into("<II", blob, slot_idx * 8, h_val, off_val)

    # ========================================================================
    # 1. GENERATE C++ HEADER: native/effect/catalog.h
    # ========================================================================
    hex_lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i : i + 16]
        hex_str = ", ".join(f"0x{b:02X}u" for b in chunk)
        hex_lines.append(f"    {hex_str},")

    header_content = f"""// ============================================================================
// AUTO-GENERATED BY native/effect/codegen_effect_catalog.py — DO NOT EDIT
// ----------------------------------------------------------------------------
// ZERO-ENUM / SELF-CONTAINED BINARY BLOB & COMPILE-TIME METAPROGRAMMING
// - 0 Enums, 0 Symbolic Schema/Param Structs, 0 Runtime JSON Parsing.
// - All 215 Effects & 2,226 Parameters are packed into a single 64B-aligned
//   self-interpreted binary image: kCatalogBlob ({len(blob)} bytes).
// - All bitfield & register meanings are documented for debugging in:
//   BIT_AND_REGISTER_DICTIONARY.txt
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <bit>
#include "../core/types.h"
#include "../core/nested_mark.h"

namespace stockcut {{
namespace effect_catalog {{

constexpr u32 kCatalogEffectCount     = 215u;
constexpr u32 kCatalogParamCount      = 2226u;
constexpr u32 kCatalogHashDirMask     = 511u;
constexpr u32 kCatalogOrdinalTableOff = 4096u;
constexpr u32 kCatalogRecordsStartOff = 4960u;
constexpr u32 kCatalogBlobSizeBytes   = {len(blob)}u;

// ============================================================================
// 1. SELF-CONTAINED BINARY CATALOG BLOB ({len(blob)} BYTES, 64B CACHELINE ALIGNED)
// ============================================================================
alignas(64) inline constexpr u8 kCatalogBlob[{len(blob)}] = {{
{chr(10).join(hex_lines)}
}};

// ============================================================================
// 2. COMPILE-TIME & RUNTIME FNV-1A HASHING (CONSTEXPR / CONSTEVAL FOLDED)
// ============================================================================
[[nodiscard]] [[gnu::always_inline]] inline constexpr u32 fnv1a_lower(const char* str) noexcept {{
    if (!str) return 0u;
    while (*str == ' ' || *str == '\\t' || *str == '\\r' || *str == '\\n') ++str;
    const char* end = str;
    while (*end != '\\0') ++end;
    while (end > str && (end[-1] == ' ' || end[-1] == '\\t' || end[-1] == '\\r' || end[-1] == '\\n')) --end;

    u32 h = 2166136261u;
    for (const char* p = str; p < end; ++p) {{
        u8 c = static_cast<u8>(*p);
        if (c >= 'A' && c <= 'Z') c = static_cast<u8>(c + ('a' - 'A'));
        h ^= static_cast<u32>(c);
        h *= 16777619u;
    }}
    return h;
}}

// ============================================================================
// 3. CONSTEXPR LITTLE-ENDIAN BLOB PRIMITIVES (CONSTANT-FOLDED BY CLANG)
// ============================================================================
[[nodiscard]] [[gnu::always_inline]] inline constexpr u32 blob_read_u32(u32 byteOff) noexcept {{
    return static_cast<u32>(kCatalogBlob[byteOff + 0]) |
           (static_cast<u32>(kCatalogBlob[byteOff + 1]) << 8) |
           (static_cast<u32>(kCatalogBlob[byteOff + 2]) << 16) |
           (static_cast<u32>(kCatalogBlob[byteOff + 3]) << 24);
}}

[[nodiscard]] [[gnu::always_inline]] inline constexpr u64 blob_read_u64(u32 byteOff) noexcept {{
    return static_cast<u64>(blob_read_u32(byteOff)) |
           (static_cast<u64>(blob_read_u32(byteOff + 4u)) << 32);
}}

[[nodiscard]] [[gnu::always_inline]] inline constexpr float blob_read_f32(u32 byteOff) noexcept {{
    return std::bit_cast<float>(blob_read_u32(byteOff));
}}

// ============================================================================
// 4. POINTER-ARITHMETIC & CONSTEXPR BLOB NAVIGATION (ZERO SYMBOLIC STRUCTS)
// ============================================================================

// Lookup Effect Record byte offset in kCatalogBlob by FNV-1a hash (O(1) open addressing)
[[nodiscard]] [[gnu::always_inline]] inline constexpr u32 blob_find_effect_offset_by_hash(u32 hash) noexcept {{
    if (hash == 0u) return 0u;
    u32 slot = hash & kCatalogHashDirMask;
    for (u32 probe = 0u; probe <= kCatalogHashDirMask; ++probe) {{
        const u32 dirOff = slot << 3;
        const u32 k = blob_read_u32(dirOff);
        if (k == 0u) return 0u;
        if (k == hash) return blob_read_u32(dirOff + 4u);
        slot = (slot + 1u) & kCatalogHashDirMask;
    }}
    return 0u;
}}

// Lookup Effect Record byte offset by ordinal index (0..214)
[[nodiscard]] [[gnu::always_inline]] inline constexpr u32 blob_effect_offset_by_ordinal(u32 idx) noexcept {{
    if (idx >= kCatalogEffectCount) return 0u;
    return blob_read_u32(kCatalogOrdinalTableOff + (idx << 2));
}}

// Read 64-bit Anonymous Algebra Word at +16 of Effect Record (Zero-Enum Fusion Signature)
[[nodiscard]] [[gnu::always_inline]] inline constexpr u64 blob_read_algebra_word(u32 recOff) noexcept {{
    if (recOff == 0u || recOff + 24u > kCatalogBlobSizeBytes) return 0ULL;
    return blob_read_u64(recOff + 16u);
}}

// Parameter records start immediately after 24-byte header (+0 headerWord, +8 defaultBits0, +16 algebraWord)
constexpr u32 kCatalogRecordParamsOffset = 24u;

}} // namespace effect_catalog
}} // namespace stockcut
"""
    OUT_HEADER.write_text(header_content, encoding="utf-8")

    # ========================================================================
    # 2. GENERATE DEBUG DICTIONARY: BIT_AND_REGISTER_DICTIONARY.txt
    # ========================================================================
    dict_lines = [
        "================================================================================",
        "STOCKCUT ENGINE — ZERO-ENUM BITFIELD & REGISTER DEBUG DICTIONARY",
        "================================================================================",
        "Tài liệu tra cứu vật lý dùng khi Debug (thay thế hoàn toàn mọi Enum trong C++).",
        "Mọi trạng thái của MediaType Block và 215 Effects (2,226 Params) đều là các dải",
        "bit trong Mask<BitBlocks> và thanh ghi float trong MultiArray<float, FloatCount>.",
        "",
        "--------------------------------------------------------------------------------",
        "PHẦN 1: QUY ƯỚC BITFIELD TRONG kCatalogBlob (native/effect/catalog.h)",
        "--------------------------------------------------------------------------------",
        "1.1. Effect Record Header Word (u64 headerWord tại byte +0 của mỗi Effect Record):",
        "  - Bits [0..7]   (8 bits) : floatCount (Số float đã căn lề bội của 4: 4..88)",
        "  - Bits [8..15]  (8 bits) : paramCount (Tổng số tham số của hiệu ứng: 0..69)",
        "  - Bits [16..17] (2 bits) : tierShift  (0=Block<16,1>, 1=Block<32,1>, 2=Block<64,2>, 3=Block<96,2>)",
        "  - Bits [18..20] (3 bits) : execMode   (0=SinglePass, 1=MultiPass, 2=Particle, 3=Audio, 4=CPU)",
        "  - Bits [21..22] (2 bits) : bitWords   (Số từ u64 cần cho tầng bits: 1 hoặc 2)",
        "  - Bit  [23]     (1 bit)  : isFill     (0=Effect, 1=Fill)",
        "  - Bits [24..27] (4 bits) : texCount   (Số texture đi kèm: 0..6)",
        "  - Bits [28..31] (4 bits) : cubeCount  (Số cubemap đi kèm: 0..6)",
        "  - Bits [32..47] (16 bits): shaderSlot (Chỉ số shader trong bảng: 0..181 hoặc 0xFFFF)",
        "  - Bits [48..63] (16 bits): defaultFloatsRelOff (Byte offset tương đối tới mảng float mặc định)",
        "",
        "1.1b. Anonymous Algebra Word (u64 algebraWord tại byte +16 của mỗi Effect Record):",
        "  - Bits [0..3]   (4 bits) : I/O ABI (bit0=ABI_UV, bit1=ABI_SAMPLE, bit2=ABI_COLOR, bit3=ABI_GEOM)",
        "  - Bits [4..10]  (7 bits) : 7 Spatial-Temporal Domains (D0_FULL..D6_AUDIO_SYNC)",
        "  - Bits [11..15] (5 bits) : CPU Pre-Fold Traits (AFFINE_2D, AFFINE_COLOR, GAUSSIAN_CONV, IDENTITY_GATE, COMMUTATIVE)",
        "  - Bits [16..23] (8 bits) : Shared Aux Blackboard Dependencies (NoiseAtlas..HistoryRing)",
        "  - Bits [24..31] (8 bits) : boundedRegAddr (Reg index of radius/size/width or 0xFF)",
        "  - Bits [32..39] (8 bits) : identityGateRegAddr (Reg index of intensity/amount gate or 0xFF)",
        "  - Bits [40..47] (8 bits) : aluWeight (Estimated GPU ALU weight 1..255)",
        "  - Bits [48..55] (8 bits) : spatialShapeClass (0=Full, 1=RadialDisk, 2=ScanStrip+Block, 3=BorderFrame, 4=IntroBar)",
        "  - Bits [56..63] (8 bits) : builtinKernelIndex (0=generic catalog, 1..24=pure GLSL snippet ordinal)",
        "",
        "1.2. Parameter Record (24 bytes tại +24 + localIdx * 24):",
        "  - Byte +0..3   (u32)  : fullHash (FNV-1a của 'group/name' viết thường)",
        "  - Byte +4..7   (u32)  : bareHash (FNV-1a của 'name' viết thường)",
        "  - Byte +8..15  (u64)  : specWord (Từ điều khiển vật lý 64-bit, chi tiết bên dưới)",
        "  - Byte +16..19 (float): minVal   (Ngưỡng dưới)",
        "  - Byte +20..23 (float): maxVal   (Ngưỡng trên)",
        "",
        "1.3. Parameter Spec Word (u64 specWord tại byte +8 của mỗi Parameter Record):",
        "  - Bit  [0]      (0x01)   : IS_BIT_LAYER (1 = Lưu ở bits[], 0 = Lưu ở reg[])",
        "  - Bit  [1]      (0x02)   : IS_RAW_U32   (1 = Ep kiểu bit_cast<float>(u32) cho Resource ID)",
        "  - Bit  [2]      (0x04)   : ROUND_INT    (1 = Làm tròn số nguyên std::round)",
        "  - Bit  [3]      (0x08)   : CLAMP        (1 = Kẹp cứng giá trị trong [minVal, maxVal])",
        "  - Bit  [4]      (0x10)   : ABOVE_ZERO   (1 = Bắt buộc > 0, chặn chia cho 0)",
        "  - Bit  [5]      (0x20)   : HAS_CYCLE    (1 = Cuộn vòng modulo fmod theo chu kỳ maxVal - minVal)",
        "  - Bit  [6]      (0x40)   : HAS_MIN      (1 = Có giới hạn minVal)",
        "  - Bit  [7]      (0x80)   : HAS_MAX      (1 = Có giới hạn maxVal)",
        "  - Bits [8..15]  (8 bits) : offset       (Bit index trong bits[] nếu Bit[0]=1, hoặc Reg index trong reg[] nếu Bit[0]=0)",
        "  - Bits [16..21] (6 bits) : width        (Độ rộng bit 1..16 nếu Bit[0]=1, hoặc số chiều vec 1..4 nếu Bit[0]=0)",
        "  - Bits [22..29] (8 bits) : depBoolBit   (Bit index của công tắc bật/tắt Group trong bits[], 0xFF = độc lập)",
        "  - Bits [30..37] (8 bits) : depEnumOff   (Bit offset của Enum điều khiển ẩn/hiện trong bits[], 0xFF = độc lập)",
        "  - Bits [38..41] (4 bits) : depEnumWid   (Độ rộng bit của Enum điều khiển trong bits[], 0 = độc lập)",
        "  - Bits [42..63] (22 bits): enumMask     (Mặt nạ bit (1 << enumVal) cho phép hiển thị tham số)",
        "",
        "--------------------------------------------------------------------------------",
        "PHẦN 2: TỪ ĐIỂN THANH GHI & BIT CHO CÁC MEDIA BLOCK (native/property/block.h)",
        "--------------------------------------------------------------------------------",
        "2.1. Dải Prefix Không Gian Chung (Mọi đối tượng hình ảnh/video/text/path đều dùng chung reg[0..8]):",
        "  - reg[0] : left (Tọa độ X trên Stage)",
        "  - reg[1] : top (Tọa độ Y trên Stage)",
        "  - reg[2] : width (Chiều rộng)",
        "  - reg[3] : height (Chiều cao)",
        "  - reg[4] : rotateDeg (Góc xoay độ)",
        "  - reg[5] : scale (Tỷ lệ phóng đại đồng nhất)",
        "  - reg[6] : opacity (Độ mờ đục 0.0 .. 1.0)",
        "  - reg[7] : anchorX (Tâm xoay X chuẩn hóa 0.0 .. 1.0)",
        "  - reg[8] : anchorY (Tâm xoay Y chuẩn hóa 0.0 .. 1.0)",
        "",
        "2.2. ImageBlock / VideoBlock = Block<16, 1> (80 bytes):",
        "  - bits[0]    : visible (1 = hiển thị, 0 = ẩn)",
        "  - bits[1]    : flipX (Lật ngang)",
        "  - bits[2]    : flipY (Lật dọc)",
        "  - bits[3..6] : blendMode (4 bits: 0=Normal, 1=Add, 2=Multiply, 3=Screen, 4=Overlay...)",
        "  - bits[7]    : loopMedia (Lặp video/gif)",
        "  - reg[0..8]  : Prefix không gian chung",
        "  - reg[9]     : spinSpeed (Tốc độ tự quay)",
        "  - reg[10]    : maskHole (Bán kính lỗ mask)",
        "  - reg[11]    : volume (Âm lượng cho Video 0.0 .. 1.0)",
        "  - reg[12..15]: tintColor RGBA (vec4) hoặc [resourceId(u32), trimInSec, trimOutSec, playbackRate]",
        "",
        "2.3. TextBlock / LyricBlock = Block<24, 1> (112 bytes):",
        "  - bits[0]    : visible",
        "  - bits[1]    : isItalic (Chữ nghiêng)",
        "  - bits[2..4] : textDecoration (3 bits: 0=None, 1=Underline, 2=LineThrough)",
        "  - bits[5]    : inheritColor",
        "  - bits[6..7] : textAlign (2 bits: 0=Left, 1=Center, 2=Right, 3=Justify)",
        "  - bits[8..9] : textTransform (2 bits: 0=None, 1=Uppercase, 2=Lowercase, 3=Capitalize)",
        "  - bits[10..13]: revealMode (4 bits hiệu ứng xuất hiện chữ)",
        "  - reg[0..8]  : Prefix không gian chung",
        "  - reg[9]     : fontSize",
        "  - reg[10]    : lineHeight",
        "  - reg[11]    : letterSpacing",
        "  - reg[12]    : strokeWidth",
        "  - reg[13]    : shadowBlur",
        "  - reg[14]    : shadowOffsetX",
        "  - reg[15]    : shadowOffsetY",
        "  - reg[16]    : color (bit_cast<u32> RGBA8888)",
        "  - reg[17]    : strokeColor (bit_cast<u32> RGBA8888)",
        "  - reg[18]    : shadowColor (bit_cast<u32> RGBA8888)",
        "  - reg[19]    : fontHash (bit_cast<u32> FNV-1a font family hash)",
        "  - reg[20]    : fontWeight (ép kiểu nguyên 100..900)",
        "  - reg[21]    : revealDurationMs",
        "  - reg[22]    : revealStaggerMs",
        "  - reg[23]    : resolvedFontFileId (bit_cast<u32>)",
        "",
        "2.4. PathBlock = Block<24, 1> (112 bytes):",
        "  - bits[0]    : visible",
        "  - bits[1]    : closedPath (Đường khép kín)",
        "  - bits[2..3] : lineCap (0=Butt, 1=Round, 2=Square)",
        "  - bits[4..5] : lineJoin (0=Miter, 1=Round, 2=Bevel)",
        "  - reg[0..8]  : Prefix không gian chung",
        "  - reg[9]     : strokeWidth",
        "  - reg[10]    : miterLimit",
        "  - reg[11]    : trimStart (0.0 .. 1.0)",
        "  - reg[12]    : trimEnd (0.0 .. 1.0)",
        "  - reg[13]    : trimOffset",
        "  - reg[14]    : fillColor (bit_cast<u32> RGBA8888)",
        "  - reg[15]    : strokeColor (bit_cast<u32> RGBA8888)",
        "  - reg[16]    : pathResourceId (bit_cast<u32>)",
        "",
        "2.5. AudioBlock = Block<16, 1> (80 bytes):",
        "  - bits[0]    : muted",
        "  - bits[1]    : loopAudio",
        "  - reg[0]     : volume (0.0 .. 1.0)",
        "  - reg[1]     : pan (-1.0 .. +1.0)",
        "  - reg[2]     : pitch (0.5 .. 2.0)",
        "  - reg[3]     : fadeInMs",
        "  - reg[4]     : fadeOutMs",
        "  - reg[5]     : audioResourceId (bit_cast<u32>)",
        "",
        "--------------------------------------------------------------------------------",
        "PHẦN 3: TỪ ĐIỂN CHI TIẾT 215 EFFECTS & FILLS (2,226 THAM SỐ TRONG kCatalogBlob)",
        "--------------------------------------------------------------------------------",
    ]

    tier_names = {0: "Block<16,1> (80B)", 1: "Block<32,1> (144B)", 2: "Block<64,2> (288B)", 3: "Block<96,2> (416B)"}

    for em in effect_records_meta:
        dict_lines.append(
            f"\n[{em['effect_idx']:03d}] {em['name']} | path='{em['res_path']}' | "
            f"blobOff={em['record_off']} (0x{em['record_off']:05X})"
        )
        dict_lines.append(
            f"      pathHash=0x{em['path_hash']:08X} | nameHash=0x{em['name_hash']:08X} | "
            f"headerWord=0x{em['header_word']:016X} | defaultBits0=0x{em['default_bits0']:016X} | "
            f"algebraWord=0x{em['algebra_word']:016X}"
        )
        dict_lines.append(
            f"      tier={tier_names[em['tier_shift']]} | floats={em['float_count']} | "
            f"params={em['param_count']} | defaultFloatsOff=+{em['default_floats_rel_off']}"
        )
        for p in em["params"]:
            is_bit = (p["trait_flags"] & 0x01) != 0
            if is_bit:
                if p["width"] == 1:
                    loc_str = f"bits[{p['offset']}] (1 bit Bool)"
                else:
                    loc_str = f"bits[{p['offset']}..{p['offset'] + p['width'] - 1}] ({p['width']} bits Enum)"
            else:
                if p["width"] == 1:
                    loc_str = f"reg[{p['offset']}] (1 float {p['ptype']})"
                else:
                    loc_str = f"reg[{p['offset']}..{p['offset'] + p['width'] - 1}] ({p['width']} floats {p['ptype']})"

            dep_parts = []
            if p["dep_bool_bit"] != 0xFF:
                dep_parts.append(f"depBool=bits[{p['dep_bool_bit']}]")
            if p["dep_enum_bit_off"] != 0xFF:
                dep_parts.append(
                    f"depEnum=bits[{p['dep_enum_bit_off']}..{p['dep_enum_bit_off'] + p['dep_enum_bit_width'] - 1}]"
                    f"(mask=0x{p['enum_allowed_mask']:X})"
                )
            dep_str = (" | " + ", ".join(dep_parts)) if dep_parts else ""

            dict_lines.append(
                f"        p[{p['local_idx']:02d}] '{p['full_path']}' -> {loc_str} | "
                f"fullHash=0x{p['full_hash']:08X} bareHash=0x{p['bare_hash']:08X} | "
                f"spec=0x{p['spec_word']:016X} | min={p['min_val']:g} max={p['max_val']:g}{dep_str}"
            )

    OUT_DICT_TXT.write_text("\n".join(dict_lines) + "\n", encoding="utf-8")
    print(
        f"Generated Zero-Enum kCatalogBlob ({len(blob)} bytes) in {OUT_HEADER.name} "
        f"and debug dictionary in {OUT_DICT_TXT.name}."
    )


if __name__ == "__main__":
    main()
