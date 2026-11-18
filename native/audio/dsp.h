// ============================================================================
// CoreEngine - Audio DSP & Fast FFT Subsystem
// ============================================================================
#pragma once

#include "types.h"
#include "core/data_structures.h"

namespace stockcut {

constexpr usize FFT_FRAME_SIZE          = 2048;
constexpr usize FFT_HALF_BINS           = FFT_FRAME_SIZE / 2;
constexpr usize FFT_LOG2_SIZE           = 11;
constexpr usize AUDIO_MAG_BUF_SIZE      = 1028;
constexpr u32   MAX_PCM_ALLOC_SAMPLES   = 50000000u;
constexpr u32   MAX_AUDIO_TRACKS        = MAX_TIMELINE_SLOTS;
constexpr u32   MAX_AUDIO_VOL_KF        = 16u;
constexpr i32   AUDIO_DRIFT_GRAVITY_MAX = 1000;

void   audio_fft_init();
float* audio_get_sample_buf();
float* audio_get_mag_buf();
void   audio_fft_transform_frame(u32 validCount);
void   audio_compute_superflux(const float* currBands, const float* prevBands, u32 numBands, float* outDiff);

struct SuperFluxParams {
    float  cutoff;      // e.g. 150 Hz
    float  sensitivity; // e.g. 1.0
    float  threshold;   // e.g. 0.2
    float  release;     // e.g. 180 ms
    double durationMs;
};

u32 audio_analyze_pcm_and_bake(
    const float* pcmSamples,
    u32 numSamples,
    u32 sampleRate,
    const SuperFluxParams& params
);

struct AudioVolumePoint {
    i32   offsetMs;
    float volume;
};

struct alignas(16) AudioVolumeEnvelope {
    float initialVolume;
    u32   count;
    MultiArray<AudioVolumePoint, MAX_AUDIO_VOL_KF> points;
};
static_assert(sizeof(AudioVolumeEnvelope) == 144, "AudioVolumeEnvelope Wasm layout changed");

enum AudioActionMask : u32 {
    AUDIO_ACT_NONE       = 0u,
    AUDIO_ACT_SEEK       = 1u << 0,
    AUDIO_ACT_SET_VOLUME = 1u << 1,
    AUDIO_ACT_PLAY       = 1u << 2,
    AUDIO_ACT_PAUSE      = 1u << 3
};

struct alignas(16) AudioSyncResult {
    u32   actionMask;
    float targetSec;
    float targetVolume;
    float driftEma;
};

struct alignas(64) AudioTrackSlot {
    u16   mediaType;
    i64   startMs;
    i64   endMs;
    float trimOffsetSec;
    float baseVolume;
    float lastAppliedVol;
    float driftEma;
    StatsEntry<i32, AUDIO_DRIFT_GRAVITY_MAX> driftGravity;
    double lastSeekWallMs;
    u32   volKfCount;
    u32   activeFlag;
    u32   lastKfCursor;
    MultiArray<i32, MAX_AUDIO_VOL_KF>        volKfOffsetMs;
    MultiArray<float, MAX_AUDIO_VOL_KF>      volKfVal;
    Sentinel<i32, MAX_AUDIO_VOL_KF, 1, 1>    bakedOffsetMs{};
    Sentinel<float, MAX_AUDIO_VOL_KF, 1, 1>  bakedVal{};
    Sentinel<float, MAX_AUDIO_VOL_KF, 1, 1>  bakedInvSpan{};
};

void                 audio_tracks_reset();
template <MediaType Type>
void                 audio_track_configure(u32 slot, i64 startMs, i64 endMs, float trimOffsetSec, float baseVolume, u32 volKfCount);
void                 audio_track_set_vol_kf(u32 slot, u32 kfIdx, i32 offsetMs, float volume);
AudioSyncResult*     audio_get_sync_result_ptr();
u32                  audio_track_sync(u32 slot, double timelineNowMs, double mediaCurrentSec, double wallNowMs, u32 isPlaying, u32 isPaused, u32 isSeeking);
AudioVolumeEnvelope* audio_get_volume_envelopes_ptr();

template <MediaType Type, bool HasKeyframes>
void timeline_build_audio_volume_envelope(
    const AudioTrackSlot& track,
    AudioVolumeEnvelope& envelope
);

} // namespace stockcut
