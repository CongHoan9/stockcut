// ============================================================================
// CoreEngine - Audio DSP & Fast FFT Implementation
// ============================================================================
#include "dsp.h"
#include "math_utils.h"
#include "script.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace stockcut {

constexpr float  AUDIO_PLAY_DRIFT_THRESHOLD_SEC  = 0.45f;
constexpr double AUDIO_PLAY_SEEK_COOLDOWN_MS     = 400.0;
constexpr float  AUDIO_SCRUB_DRIFT_THRESHOLD_SEC = 0.05f;
constexpr double AUDIO_SCRUB_SEEK_COOLDOWN_MS    = 90.0;
constexpr double AUDIO_INITIAL_SEEK_WALL_MS      = -10000.0;
constexpr u32    SUPERFLUX_ANALYSIS_FPS          = 100u;
constexpr float  CONCERT_PITCH_A4_HZ             = 440.0f;
constexpr u32    MAX_LOG_BANDS                   = 64u;
constexpr u32    MAX_FILTER_WEIGHTS              = 128u;
constexpr u32    MAX_FREQ_SCRATCH                = 128u;

template <usize FFT_SIZE = FFT_FRAME_SIZE>
class FastFFT2048Internal {
public:
    alignas(16) MultiArray<float, FFT_SIZE / 2> cosTable{};
    alignas(16) MultiArray<float, FFT_SIZE / 2> sinTable{};
    alignas(16) MultiArray<float, FFT_SIZE>     hannWindow{};
    MultiArray<u16, FFT_SIZE>                   bitRev{};

    void init() {
        for (usize i = 0; i < FFT_SIZE; ++i) {
            u16 rev = 0, x = static_cast<u16>(i);
            for (usize j = 0; j < FFT_LOG2_SIZE; ++j) {
                rev = static_cast<u16>((rev << 1) | (x & 1u));
                x >>= 1;
            }
            bitRev[i] = rev;
            hannWindow[i] = 0.5f * (1.0f - static_cast<float>(std::cos(TWO_PI_F64 * static_cast<double>(i) / static_cast<double>(FFT_SIZE))));
        }
        for (usize i = 0; i < FFT_SIZE / 2; ++i) {
            const double theta = -TWO_PI_F64 * static_cast<double>(i) / static_cast<double>(FFT_SIZE);
            cosTable[i] = static_cast<float>(std::cos(theta));
            sinTable[i] = static_cast<float>(std::sin(theta));
        }
    }

    void transform(const float* __restrict samples, usize validCount, float* __restrict outMag) const {
        alignas(16) MultiArray<float, FFT_SIZE> re;
        alignas(16) MultiArray<float, FFT_SIZE> im;
        for (usize i = 0; i < FFT_SIZE; ++i) {
            const usize rev = bitRev[i];
            re[rev] = (i < validCount) ? (samples[i] * hannWindow[i]) : 0.0f;
            im[rev] = 0.0f;
        }
        for (usize halfSize = 1; halfSize < FFT_SIZE; halfSize <<= 1) {
            const usize step = halfSize << 1;
            const usize tableStep = FFT_SIZE / step;
            for (usize i = 0; i < FFT_SIZE; i += step) {
                for (usize j = 0; j < halfSize; ++j) {
                    const usize k = j * tableStep;
                    const float c = cosTable[k];
                    const float s = sinTable[k];
                    const float tr = re[i + j + halfSize] * c - im[i + j + halfSize] * s;
                    const float ti = re[i + j + halfSize] * s + im[i + j + halfSize] * c;
                    re[i + j + halfSize] = re[i + j] - tr;
                    im[i + j + halfSize] = im[i + j] - ti;
                    re[i + j] += tr;
                    im[i + j] += ti;
                }
            }
        }
#if HAS_WASM_SIMD
        for (usize i = 0; i < FFT_SIZE / 2; i += 4) {
            const v128_t r = wasm_v128_load(&re[i]);
            const v128_t m = wasm_v128_load(&im[i]);
            const v128_t mag_sq = wasm_f32x4_add(wasm_f32x4_mul(r, r), wasm_f32x4_mul(m, m));
            wasm_v128_store(&outMag[i], wasm_f32x4_sqrt(mag_sq));
        }
        outMag[FFT_SIZE / 2] = sqrt_f32(re[FFT_SIZE / 2] * re[FFT_SIZE / 2] + im[FFT_SIZE / 2] * im[FFT_SIZE / 2]);
#else
        for (usize i = 0; i <= FFT_SIZE / 2; ++i) {
            outMag[i] = sqrt_f32(re[i] * re[i] + im[i] * im[i]);
        }
#endif
    }
};

alignas(64) static FastFFT2048Internal<FFT_FRAME_SIZE> g_fft;
alignas(16) static MultiArray<float, AUDIO_MAG_BUF_SIZE> g_magBuf{};
alignas(16) static MultiArray<float, FFT_FRAME_SIZE>     g_sampleBuf{};

void audio_fft_init() {
    g_fft.init();
}

float* audio_get_sample_buf() {
    return g_sampleBuf.data();
}

float* audio_get_mag_buf() {
    return g_magBuf.data();
}

void audio_fft_transform_frame(u32 validCount) {
    g_fft.transform(g_sampleBuf.data(), validCount, g_magBuf.data());
}

[[nodiscard]] static inline float compute_superflux_sum(const float* currBands, const float* prevBands, u32 numBands) noexcept {
    if (!currBands || !prevBands || numBands == 0u) return 0.0f;
    float diffSum = 0.0f;
    const usize lastBand = static_cast<usize>(numBands - 1u);
    for (usize b = 0; b < numBands; ++b) {
        const usize b0 = (b > 0) ? (b - 1) : 0;
        const usize b1 = min(lastBand, b + 1);
        const float prevMax = max(prevBands[b0], max(prevBands[b], prevBands[b1]));
        const float d = currBands[b] - prevMax;
        diffSum += max(0.0f, d);
    }
    return diffSum;
}

void audio_compute_superflux(const float* currBands, const float* prevBands, u32 numBands, float* outDiff) {
    const float diffSum = compute_superflux_sum(currBands, prevBands, numBands);
    if (outDiff) *outDiff = diffSum;
}

// ============================================================================
// C++ NATIVE AUDIO TRACK MANAGER & <T> TEMPLATE DRIFT SYNCHRONIZER
// ============================================================================
alignas(64) static MultiArray<AudioTrackSlot, MAX_AUDIO_TRACKS>      g_audioTracks{};
alignas(16) static AudioSyncResult                                    g_audioSyncResult{};
alignas(16) static MultiArray<AudioVolumeEnvelope, MAX_AUDIO_TRACKS> g_audioVolumeEnvelopes{};

static void bake_audio_volume_sentinels(AudioTrackSlot& tr) noexcept {
    const u32 n = (tr.volKfCount <= MAX_AUDIO_VOL_KF) ? tr.volKfCount : MAX_AUDIO_VOL_KF;
    if (n == 0u) {
        tr.bakedOffsetMs.left_sentinel()   = KF_GATEKEEPER_MIN_I32;
        tr.bakedOffsetMs.right_sentinel(0) = KF_GATEKEEPER_MAX_I32;
        tr.bakedVal.left_sentinel()        = tr.baseVolume;
        tr.bakedVal.right_sentinel(0)      = tr.baseVolume;
        tr.bakedInvSpan.left_sentinel()    = 0.0f;
        tr.bakedInvSpan.right_sentinel(0)  = 0.0f;
        tr.lastKfCursor                    = 0u;
        return;
    }
    const i32* srcOff   = tr.volKfOffsetMs.data();
    const float* srcVal = tr.volKfVal.data();
    i32* dstOff         = tr.bakedOffsetMs.data();
    float* dstVal       = tr.bakedVal.data();
    float* dstInv       = tr.bakedInvSpan.data();
    for (u32 k = 0; k < n; ++k) {
        dstOff[k] = srcOff[k];
        dstVal[k] = srcVal[k] * tr.baseVolume;
        if (k + 1u < n) {
            const i32 span = srcOff[k + 1u] - srcOff[k];
            dstInv[k] = (span > 0) ? (1.0f / static_cast<float>(span)) : 0.0f;
        } else {
            dstInv[k] = 0.0f;
        }
    }
    tr.bakedOffsetMs.left_sentinel()   = KF_GATEKEEPER_MIN_I32;
    tr.bakedVal.left_sentinel()        = dstVal[0];
    tr.bakedInvSpan.left_sentinel()    = 0.0f;
    tr.bakedOffsetMs.right_sentinel(n) = KF_GATEKEEPER_MAX_I32;
    tr.bakedVal.right_sentinel(n)      = dstVal[n - 1u];
    tr.bakedInvSpan.right_sentinel(n)  = 0.0f;
    if (tr.lastKfCursor > n) tr.lastKfCursor = 0u;
}

static void update_audio_volume_envelope(u32 slot) {
    AudioTrackSlot& track = g_audioTracks[slot];
    bake_audio_volume_sentinels(track);
    AudioVolumeEnvelope& envelope = g_audioVolumeEnvelopes[slot];
    if (!track.activeFlag) {
        envelope.initialVolume = 0.0f;
        envelope.count = 0u;
        return;
    }
    const bool hasKf = (track.volKfCount > 0u);
    if (track.mediaType == static_cast<u16>(MediaType::Audio)) {
        hasKf ? timeline_build_audio_volume_envelope<MediaType::Audio, true>(track, envelope)
              : timeline_build_audio_volume_envelope<MediaType::Audio, false>(track, envelope);
    } else if (track.mediaType == static_cast<u16>(MediaType::Video)) {
        hasKf ? timeline_build_audio_volume_envelope<MediaType::Video, true>(track, envelope)
              : timeline_build_audio_volume_envelope<MediaType::Video, false>(track, envelope);
    } else {
        envelope.initialVolume = 0.0f;
        envelope.count = 0u;
    }
}

void audio_tracks_reset() {
    g_audioTracks.clear();
    std::memset(&g_audioSyncResult, 0, sizeof(g_audioSyncResult));
    g_audioVolumeEnvelopes.clear();
    for (AudioTrackSlot* tr = g_audioTracks.begin(), *const trEnd = g_audioTracks.end(); tr < trEnd; ++tr) {
        tr->baseVolume     = 1.0f;
        tr->lastAppliedVol = -1.0f;
        tr->lastSeekWallMs = AUDIO_INITIAL_SEEK_WALL_MS;
        bake_audio_volume_sentinels(*tr);
    }
}

inline AudioTrackSlot& audio_track_slot(u32 slot) noexcept {
    return g_audioTracks[slot & (MAX_AUDIO_TRACKS - 1u)];
}

template <MediaType Type>
void audio_track_configure(u32 slot, i64 startMs, i64 endMs, float trimOffsetSec, float baseVolume, u32 volKfCount) {
    static_assert(Type == MediaType::Audio || Type == MediaType::Video);
    AudioTrackSlot& tr = audio_track_slot(slot);
    tr.mediaType      = static_cast<u16>(Type);
    tr.startMs        = startMs;
    tr.endMs          = endMs;
    tr.trimOffsetSec  = trimOffsetSec;
    tr.baseVolume     = baseVolume;
    tr.volKfCount     = (volKfCount > MAX_AUDIO_VOL_KF) ? MAX_AUDIO_VOL_KF : volKfCount;
    tr.activeFlag     = 1u;
    tr.lastAppliedVol = -1.0f;
    update_audio_volume_envelope(slot & (MAX_AUDIO_TRACKS - 1u));
}

template void audio_track_configure<MediaType::Audio>(u32, i64, i64, float, float, u32);
template void audio_track_configure<MediaType::Video>(u32, i64, i64, float, float, u32);

void audio_track_set_vol_kf(u32 slot, u32 kfIdx, i32 offsetMs, float volume) {
    AudioTrackSlot& tr = audio_track_slot(slot);
    const u32 safeKf = kfIdx & (MAX_AUDIO_VOL_KF - 1u);
    tr.volKfOffsetMs[safeKf] = offsetMs;
    tr.volKfVal[safeKf]      = volume;
    update_audio_volume_envelope(slot & (MAX_AUDIO_TRACKS - 1u));
}

AudioSyncResult* audio_get_sync_result_ptr() {
    return &g_audioSyncResult;
}

AudioVolumeEnvelope* audio_get_volume_envelopes_ptr() {
    return g_audioVolumeEnvelopes.data();
}

inline float evaluate_audio_volume(AudioTrackSlot& tr, double timelineNowMs) noexcept {
    const i32 relMs = static_cast<i32>(timelineNowMs - static_cast<double>(tr.startMs));
    const u32 idx = tr.bakedOffsetMs.walk_cursor(tr.lastKfCursor, relMs, [](i32 t) noexcept { return t; });
    tr.lastKfCursor = idx;
    const float alpha = static_cast<float>(relMs - tr.bakedOffsetMs.sentinel_at(idx)) * tr.bakedInvSpan.sentinel_at(idx);
    return tr.bakedVal.sentinel_at(idx) + (tr.bakedVal.sentinel_at(idx + 1u) - tr.bakedVal.sentinel_at(idx)) * alpha;
}

template <bool PlayMode, bool IsPaused, bool IsSeeking>
inline u32 audio_sync_slot_T(
    AudioTrackSlot& tr,
    double timelineNowMs,
    double mediaCurrentSec,
    double wallNowMs
) {
    u32 mask = AUDIO_ACT_NONE;
    const double relSec = (timelineNowMs - static_cast<double>(tr.startMs)) * 0.001;
    const float targetSec = static_cast<float>((relSec > 0.0 ? relSec : 0.0) + static_cast<double>(tr.trimOffsetSec));
    const float targetVol = evaluate_audio_volume(tr, timelineNowMs);
    if (abs_val(targetVol - tr.lastAppliedVol) > DELTA_EPSILON) {
        tr.lastAppliedVol = targetVol;
        mask |= AUDIO_ACT_SET_VOLUME;
    }
    const float rawDrift = targetSec - static_cast<float>(mediaCurrentSec);
    if constexpr (PlayMode) {
        const i32 driftDeltaMs = static_cast<i32>(rawDrift * 1000.0f);
        tr.driftGravity << driftDeltaMs;
        tr.driftEma = static_cast<float>(static_cast<i32>(tr.driftGravity)) * 0.001f;
        if constexpr (!IsSeeking) {
            if (abs_val(rawDrift) > AUDIO_PLAY_DRIFT_THRESHOLD_SEC &&
                (wallNowMs - tr.lastSeekWallMs) > AUDIO_PLAY_SEEK_COOLDOWN_MS) {
                tr.lastSeekWallMs = wallNowMs;
                tr.driftGravity = 0;
                tr.driftEma = 0.0f;
                mask |= AUDIO_ACT_SEEK;
            }
        }
        if constexpr (IsPaused) {
            mask |= AUDIO_ACT_PLAY;
        }
    } else {
        tr.driftGravity = 0;
        tr.driftEma = 0.0f;
        if constexpr (!IsPaused) {
            mask |= AUDIO_ACT_PAUSE;
        }
        if constexpr (!IsSeeking) {
            if (abs_val(rawDrift) > AUDIO_SCRUB_DRIFT_THRESHOLD_SEC &&
                (wallNowMs - tr.lastSeekWallMs) > AUDIO_SCRUB_SEEK_COOLDOWN_MS) {
                tr.lastSeekWallMs = wallNowMs;
                mask |= AUDIO_ACT_SEEK;
            }
        }
    }
    g_audioSyncResult.actionMask   = mask;
    g_audioSyncResult.targetSec    = targetSec;
    g_audioSyncResult.targetVolume = targetVol;
    g_audioSyncResult.driftEma     = tr.driftEma;
    return mask;
}

using AudioSyncFn = u32 (*)(AudioTrackSlot&, double, double, double);
static constexpr MultiArray<AudioSyncFn, 8> kAudioSyncTable = {{
    &audio_sync_slot_T<false, false, false>,
    &audio_sync_slot_T<false, false, true>,
    &audio_sync_slot_T<false, true,  false>,
    &audio_sync_slot_T<false, true,  true>,
    &audio_sync_slot_T<true,  false, false>,
    &audio_sync_slot_T<true,  false, true>,
    &audio_sync_slot_T<true,  true,  false>,
    &audio_sync_slot_T<true,  true,  true>
}};

u32 audio_track_sync(u32 slot, double timelineNowMs, double mediaCurrentSec, double wallNowMs, u32 isPlaying, u32 isPaused, u32 isSeeking) {
    const u32 modeIdx = ((isPlaying != 0u) ? 4u : 0u) |
                        ((isPaused != 0u)  ? 2u : 0u) |
                        ((isSeeking != 0u) ? 1u : 0u);
    return kAudioSyncTable[modeIdx](audio_track_slot(slot), timelineNowMs, mediaCurrentSec, wallNowMs);
}

// ============================================================================
// NATIVE MADMOM SUPERFLUX DSP PIPELINE (100% C++ INTERNAL ENGINE)
// ============================================================================
struct LogFilter {
    u16 startBin;
    u16 numBins;
    MultiArray<float, MAX_FILTER_WEIGHTS> weights{};
};

struct LogFilterbank {
    u32 count = 0;
    MultiArray<LogFilter, MAX_LOG_BANDS> filters{};

    void build(float sr, u32 fftSize, float fmin, float fmax, u32 bandsPerOct) {
        count = 0;
        const int left  = static_cast<int>(std::floor(std::log2(fmin / CONCERT_PITCH_A4_HZ) * static_cast<float>(bandsPerOct)));
        const int right = static_cast<int>(std::ceil(std::log2(fmax / CONCERT_PITCH_A4_HZ) * static_cast<float>(bandsPerOct)));
        MultiArray<float, MAX_FREQ_SCRATCH> freqs;
        u32 freqCount = 0;
        for (int i = left; i <= right && freqCount < (MAX_FREQ_SCRATCH - 2u); ++i) {
            const float f = CONCERT_PITCH_A4_HZ * std::pow(2.0f, static_cast<float>(i) / static_cast<float>(bandsPerOct));
            if (f >= fmin && f <= fmax) {
                freqs[freqCount++] = f;
            }
        }
        if (freqCount == 0u) {
            freqs[freqCount++] = fmin;
            freqs[freqCount++] = fmax;
        }
        const float binFreqStep = sr / static_cast<float>(fftSize);
        MultiArray<u32, MAX_FREQ_SCRATCH> uniqueBins;
        u32 ubCount = 0;
        for (u32 i = 0; i < freqCount; ++i) {
            const u32 b = static_cast<u32>(std::round(freqs[i] / binFreqStep));
            if (ubCount == 0u || b > uniqueBins[ubCount - 1u]) {
                uniqueBins[ubCount++] = b;
            }
        }
        if (ubCount < 3u) {
            ubCount = 0u;
            uniqueBins[ubCount++] = max(1u, static_cast<u32>(std::floor(fmin / binFreqStep)));
            uniqueBins[ubCount++] = max(2u, static_cast<u32>(std::round((fmin + fmax) * 0.5f / binFreqStep)));
            uniqueBins[ubCount++] = max(3u, static_cast<u32>(std::ceil(fmax / binFreqStep)));
        }
        for (u32 i = 0; i + 2u < ubCount && count < MAX_LOG_BANDS; ++i) {
            const u32 start  = uniqueBins[i];
            const u32 center = uniqueBins[i + 1u];
            const u32 stop   = uniqueBins[i + 2u];
            if (stop <= start || center <= start || stop <= center) continue;
            const u32 len = min(stop - start, MAX_FILTER_WEIGHTS);
            LogFilter& filt = filters[count];
            filt.startBin = static_cast<u16>(start);
            filt.numBins  = static_cast<u16>(len);
            float total = 0.0f;
            for (u32 k = start; k < center && (k - start) < len; ++k) {
                const float w = static_cast<float>(k - start) / static_cast<float>(center - start);
                filt.weights[k - start] = w;
                total += w;
            }
            for (u32 k = center; k < stop && (k - start) < len; ++k) {
                const float w = static_cast<float>(stop - k) / static_cast<float>(stop - center);
                filt.weights[k - start] = w;
                total += w;
            }
            if (total > 0.0f) {
                const float invTotal = 1.0f / total;
                for (u32 j = 0; j < len; ++j) {
                    filt.weights[j] *= invTotal;
                }
                ++count;
            }
        }
    }
};

u32 audio_analyze_pcm_and_bake(
    const float* pcmSamples,
    u32 numSamples,
    u32 sampleRate,
    const SuperFluxParams& params
) {
    if (!pcmSamples || numSamples == 0u || sampleRate == 0u) return 0u;
    audio_fft_init();
    const u32 hopSize = max(1u, static_cast<u32>(std::round(static_cast<float>(sampleRate) / static_cast<float>(SUPERFLUX_ANALYSIS_FPS))));
    const u32 numFrames = min(MAX_SCRIPT_SAMPLES, static_cast<u32>(numSamples / hopSize));
    if (numFrames == 0u) return 0u;

    const float fmin = 30.0f;
    const float fmax = min(static_cast<float>(sampleRate) * 0.45f, max(120.0f, params.cutoff > 0.0f ? params.cutoff : 180.0f));
    LogFilterbank bassFilterbank;
    bassFilterbank.build(static_cast<float>(sampleRate), static_cast<u32>(FFT_FRAME_SIZE), fmin, fmax, 12u);
    const u32 numBands = bassFilterbank.count;

    LogFilterbank midFilterbank;
    midFilterbank.build(static_cast<float>(sampleRate), static_cast<u32>(FFT_FRAME_SIZE), 350.0f, 2200.0f, 4u);
    const u32 numMidBands = midFilterbank.count;

    static MultiArray<float, MAX_SCRIPT_SAMPLES> s_superflux{};
    static MultiArray<float, MAX_SCRIPT_SAMPLES> s_midFlux{};
    static MultiArray<float, MAX_SCRIPT_SAMPLES> s_midLog{};
    static MultiArray<float, MAX_LOG_BANDS>      s_currBands{};
    static MultiArray<float, MAX_LOG_BANDS>      s_prevBands{};
    static MultiArray<u8, MAX_SCRIPT_SAMPLES>    s_isHit{};

    std::memset(s_superflux.data(), 0, sizeof(float) * numFrames);
    std::memset(s_midFlux.data(), 0, sizeof(float) * numFrames);
    std::memset(s_midLog.data(), 0, sizeof(float) * numFrames);
    s_prevBands.clear();
    std::memset(s_isHit.data(), 0, sizeof(u8) * numFrames);

    float maxFlux = 1.0f;
    for (u32 t = 0; t < numFrames; ++t) {
        const u32 sampleOffset = t * hopSize;
        const u32 remain = (sampleOffset < numSamples) ? (numSamples - sampleOffset) : 0u;
        const u32 valid  = min(static_cast<u32>(FFT_FRAME_SIZE), remain);
        g_fft.transform(pcmSamples + sampleOffset, valid, g_magBuf.data());

        for (u32 b = 0; b < numBands; ++b) {
            const LogFilter& filt = bassFilterbank.filters[b];
            float sum = 0.0f;
            for (u32 k = 0; k < filt.numBins; ++k) {
                sum += g_magBuf[filt.startBin + k] * filt.weights[k];
            }
            s_currBands[b] = std::log10(1.0f + sum * 10.0f);
        }

        float midSum = 0.0f;
        for (u32 b = 0; b < numMidBands; ++b) {
            const LogFilter& filt = midFilterbank.filters[b];
            for (u32 k = 0; k < filt.numBins; ++k) {
                midSum += g_magBuf[filt.startBin + k] * filt.weights[k];
            }
        }
        s_midLog[t] = std::log10(1.0f + midSum * 5.0f);

        if (t > 0u) {
            const float diffSum = compute_superflux_sum(s_currBands.data(), s_prevBands.data(), numBands);
            s_superflux[t] = diffSum;
            if (diffSum > maxFlux) maxFlux = diffSum;
            const float md = s_midLog[t] - s_midLog[t - 1u];
            s_midFlux[t] = max(0.0f, md);
        }
        std::memcpy(s_prevBands.data(), s_currBands.data(), sizeof(float) * numBands);
    }

    const float sens      = clamp(params.sensitivity, 0.1f, 10.0f);
    const float userTh    = clamp(params.threshold, 0.01f, 0.95f);
    const float threshold = (0.22f / sqrt_f32(sens)) * (1.0f + (userTh - 0.2f) * 1.5f);
    const i32 preAvg      = static_cast<i32>(std::round(0.12f * static_cast<float>(SUPERFLUX_ANALYSIS_FPS)));
    const i32 postAvg     = static_cast<i32>(std::round(0.04f * static_cast<float>(SUPERFLUX_ANALYSIS_FPS)));
    const i32 preMax      = static_cast<i32>(std::round(0.03f * static_cast<float>(SUPERFLUX_ANALYSIS_FPS)));
    const i32 postMax     = static_cast<i32>(std::round(0.03f * static_cast<float>(SUPERFLUX_ANALYSIS_FPS)));
    const i32 combine     = max(5, static_cast<i32>(std::round(0.06f * static_cast<float>(SUPERFLUX_ANALYSIS_FPS))));

    auto* script = get_active_music_script();
    if (!script) return 0u;

    u32 count = 0u;
    i32 lastHit = -combine;
    for (u32 t = 0; t < numFrames; ++t) {
        const i32 tLo = max(0, static_cast<i32>(t) - preAvg);
        const i32 tHi = min(static_cast<i32>(numFrames - 1u), static_cast<i32>(t) + postAvg);
        float sumAvg = 0.0f;
        for (i32 j = tLo; j <= tHi; ++j) {
            sumAvg += s_superflux[static_cast<u32>(j)];
        }
        const float avg = sumAvg / static_cast<float>(tHi - tLo + 1);
        if (s_superflux[t] >= avg + threshold) {
            const i32 mLo = max(0, static_cast<i32>(t) - preMax);
            const i32 mHi = min(static_cast<i32>(numFrames - 1u), static_cast<i32>(t) + postMax);
            bool isMax = true;
            for (i32 j = mLo; j <= mHi; ++j) {
                if (s_superflux[static_cast<u32>(j)] > s_superflux[t]) {
                    isMax = false;
                    break;
                }
            }
            const bool passesVocal = (s_superflux[t] > s_midFlux[t] * 0.85f) || (s_midFlux[t] < 0.50f);
            if (isMax && passesVocal && (static_cast<i32>(t) - lastHit >= combine)) {
                ++count;
                lastHit = static_cast<i32>(t);
                s_isHit[t] = 1u;
            }
        }
        script->hitIndices[t] = static_cast<u16>(count & INVALID_U16);
    }

    const float release    = params.release > 0.0f ? params.release : 180.0f;
    const float decayMs    = clamp(release * 0.45f, 35.0f, 250.0f);
    const float hopSec     = static_cast<float>(hopSize) / static_cast<float>(sampleRate);
    const float decayCoeff = std::exp(-hopSec * 1000.0f / decayMs);
    float pulse = 0.0f;
    for (u32 t = 0; t < numFrames; ++t) {
        pulse = s_isHit[t] ? 1.0f : (pulse * decayCoeff);
        const float continuous = s_superflux[t] / (maxFlux > 0.0f ? maxFlux : 1.0f);
        script->levels[t] = min(1.0f, pulse * 0.85f + continuous * 0.15f);
    }

    const double finalDurationMs = params.durationMs > 0.0 ? params.durationMs : (numFrames * hopSec * 1000.0);
    script->bake(script->levels.data(), script->hitIndices.data(), numFrames, hopSec, finalDurationMs);
    return numFrames;
}

template <MediaType Type, bool HasKeyframes>
void timeline_build_audio_volume_envelope(
    const AudioTrackSlot& track,
    AudioVolumeEnvelope& envelope
) {
    static_assert(Type == MediaType::Audio || Type == MediaType::Video);
    const u32 count = HasKeyframes
        ? (track.volKfCount < MAX_AUDIO_VOL_KF ? track.volKfCount : MAX_AUDIO_VOL_KF)
        : 0u;
    envelope.initialVolume = track.baseVolume;
    envelope.count = count;
    if constexpr (HasKeyframes) {
        if (count > 0u && track.volKfOffsetMs[0] == 0) {
            envelope.initialVolume = track.volKfVal[0] * track.baseVolume;
        }
        const i32* offPtr = track.volKfOffsetMs.data();
        const float* valPtr = track.volKfVal.data();
        AudioVolumePoint* dstPt = envelope.points.data();
        for (const i32* const offEnd = offPtr + count; offPtr < offEnd; ++offPtr, ++valPtr, ++dstPt) {
            dstPt->offsetMs = *offPtr;
            dstPt->volume = *valPtr * track.baseVolume;
        }
    }
}

template void timeline_build_audio_volume_envelope<MediaType::Audio, false>(
    const AudioTrackSlot&, AudioVolumeEnvelope&);
template void timeline_build_audio_volume_envelope<MediaType::Audio, true>(
    const AudioTrackSlot&, AudioVolumeEnvelope&);
template void timeline_build_audio_volume_envelope<MediaType::Video, false>(
    const AudioTrackSlot&, AudioVolumeEnvelope&);
template void timeline_build_audio_volume_envelope<MediaType::Video, true>(
    const AudioTrackSlot&, AudioVolumeEnvelope&);

} // namespace stockcut

extern "C" {

WASM_EXPORT(wasm_audio_alloc_pcm)
float* wasm_audio_alloc_pcm(stockcut::u32 sampleCount) {
    if (sampleCount == 0u || sampleCount > stockcut::MAX_PCM_ALLOC_SAMPLES) return nullptr;
    return static_cast<float*>(std::malloc(sampleCount * sizeof(float)));
}

WASM_EXPORT(wasm_audio_free_pcm)
void wasm_audio_free_pcm(float* ptr) {
    if (ptr) std::free(ptr);
}

WASM_EXPORT(wasm_audio_analyze_and_bake)
stockcut::u32 wasm_audio_analyze_and_bake(
    const float* pcmSamples,
    stockcut::u32 numSamples,
    stockcut::u32 sampleRate,
    float cutoff,
    float sensitivity,
    float threshold,
    float release,
    double durationMs
) {
    stockcut::SuperFluxParams params{cutoff, sensitivity, threshold, release, durationMs};
    return stockcut::audio_analyze_pcm_and_bake(pcmSamples, numSamples, sampleRate, params);
}

} // extern "C"
