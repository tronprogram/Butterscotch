#include "wii_audio_system.h"
#include "data_win.h"
#include "utils.h"
#include "common.h"
#include "binary_utils.h"
#include "stdio_compat.h"
#include "string_compat.h"

#include <aesndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "stb_ds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define WII_MAX_VOICES 16
#define WII_MAX_AUDIO_STREAMS 32
#define WII_SOUND_INSTANCE_ID_BASE 100000
#define WII_AUDIO_STREAM_INDEX_BASE 300000
#define WII_STREAM_FRAMES 4096
#define WII_PCM_ALIGN 32

typedef struct {
    bool active;
    char* filePath;
    float initialGain;
    float initialPitch;
} WiiAudioStreamEntry;

typedef struct {
    int32_t soundIndex;
    int16_t* pcm; // 32-byte aligned
    uint32_t sampleCount; // total interleaved samples
    int channels;
    uint32_t sampleRate;
} DecodedSound;

typedef struct VoiceSlot {
    int32_t soundIndex;
    AESNDPB* voice;
    int16_t* pcm; // non-stream one-shot / owned by cache
    uint32_t sampleCount;
    int channels;
    uint32_t sampleRate;
    bool looping;
    bool active;
    bool muted;
    bool paused;
    float gain;
    float pitch;
    int32_t instanceId;
    int32_t priority;

    bool streaming;
    stb_vorbis* vorbis;
    int16_t* streamBuf[2];
    uint32_t streamBufBytes[2];
    volatile int playBuf;
    volatile int readyBuf; // -1 = none waiting
    volatile bool needRefill[2];
    volatile bool streamEnded;
    volatile bool stoppedFlag;
    float streamLengthSeconds;
    struct WiiAudioSystem* owner;
} VoiceSlot;

struct WiiAudioSystem {
    AudioSystem base;
    FileSystem* fileSystem;
    VoiceSlot voices[WII_MAX_VOICES];
    WiiAudioStreamEntry streams[WII_MAX_AUDIO_STREAMS];
    DecodedSound* cache;
    float masterGain;
    bool muted;
    bool suspended;
    volatile uint32_t streamUnderruns;
};

static int16_t* allocAlignedPcm(uint32_t sampleCount) {
    size_t bytes = (size_t)sampleCount * sizeof(int16_t);
    // AESND copies from MEM; keep buffers 32-byte aligned and padded.
    size_t padded = (bytes + (WII_PCM_ALIGN - 1)) & ~(size_t)(WII_PCM_ALIGN - 1);
    int16_t* pcm = (int16_t*)memalign(WII_PCM_ALIGN, padded);
    if (!pcm) {
        logError("Audio: memalign(%d, %zu) failed\n", WII_PCM_ALIGN, padded);
        abort();
    }
    if (padded > bytes) memset((uint8_t*)pcm + bytes, 0, padded - bytes);
    return pcm;
}

static void flushPcm(const void* pcm, uint32_t byteLen) {
    if (!pcm || byteLen == 0) return;
    uint32_t padded = (byteLen + (WII_PCM_ALIGN - 1)) & ~(uint32_t)(WII_PCM_ALIGN - 1);
    DCFlushRange((void*)pcm, padded);
}

static bool soundInAudo(const Sound* sound) {
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    return !isRegular || isEmbedded || isCompressed;
}

static char* resolveExternalPath(WiiAudioSystem* sys, Sound* sound) {
    if (!sys->fileSystem) return nullptr;
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    bool hasExtension = (strchr(file, '.') != nullptr);
    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }
    return sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
}

static bool parseWav(const uint8_t* data, uint32_t size,
                     int16_t** outPcm, uint32_t* outCount,
                     int* outChannels, uint32_t* outRate) {
    if (size < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0) return false;
    if (memcmp(data + 8, "WAVE", 4) != 0) return false;

    uint32_t pos = 12;
    uint16_t channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* pcmData = NULL;
    uint32_t pcmDataSize = 0;

    while (pos + 8 <= size) {
        char chunkId[4];
        memcpy(chunkId, data + pos, 4);
        uint32_t chunkSize = (uint32_t)data[pos + 4]
            | ((uint32_t)data[pos + 5] << 8)
            | ((uint32_t)data[pos + 6] << 16)
            | ((uint32_t)data[pos + 7] << 24);
        pos += 8;
        if (memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint16_t audioFmt = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
            if (audioFmt != 1) return false;
            channels = (uint16_t)data[pos + 2] | ((uint16_t)data[pos + 3] << 8);
            sampleRate = (uint32_t)data[pos + 4] | ((uint32_t)data[pos + 5] << 8)
                | ((uint32_t)data[pos + 6] << 16) | ((uint32_t)data[pos + 7] << 24);
            bitsPerSample = (uint16_t)data[pos + 14] | ((uint16_t)data[pos + 15] << 8);
        } else if (memcmp(chunkId, "data", 4) == 0) {
            pcmData = data + pos;
            pcmDataSize = chunkSize;
        }
        pos += (chunkSize + 1) & ~1u;
        if (pos > size) break;
    }

    if (!pcmData || channels == 0 || bitsPerSample != 16) return false;

    uint32_t sampleCount = pcmDataSize / 2;
    int16_t* pcm = allocAlignedPcm(sampleCount);
    memcpy(pcm, pcmData, sizeof(int16_t) * sampleCount);

#if defined(IS_BIG_ENDIAN)
    for (uint32_t i = 0; i < sampleCount; i++) {
        pcm[i] = (int16_t)BinaryUtils_bswap16((uint16_t)pcm[i]);
    }
#endif

    flushPcm(pcm, sampleCount * sizeof(int16_t));
    *outPcm = pcm;
    *outCount = sampleCount;
    *outChannels = (int)channels;
    *outRate = sampleRate;
    return true;
}

static bool decodeMemoryAudio(const uint8_t* data, uint32_t size,
                              int16_t** outPcm, uint32_t* outCount,
                              int* outChannels, uint32_t* outRate) {
    if (size >= 4 && memcmp(data, "RIFF", 4) == 0) {
        return parseWav(data, size, outPcm, outCount, outChannels, outRate);
    }

    int channels = 0;
    int sampleRate = 0;
    int16_t* decoded = NULL;
    int samplesDecoded = stb_vorbis_decode_memory(data, (int)size, &channels, &sampleRate, &decoded);
    if (samplesDecoded <= 0 || decoded == NULL) return false;

    uint32_t sampleCount = (uint32_t)(samplesDecoded * channels);
    int16_t* pcm = allocAlignedPcm(sampleCount);
    memcpy(pcm, decoded, sizeof(int16_t) * sampleCount);
    free(decoded);
    flushPcm(pcm, sampleCount * sizeof(int16_t));

    *outPcm = pcm;
    *outCount = sampleCount;
    *outChannels = channels;
    *outRate = (uint32_t)sampleRate;
    return true;
}

static DataWin* audioGroupOrNull(WiiAudioSystem* sys, int32_t groupIndex) {
    if (groupIndex < 0 || groupIndex >= (int32_t)arrlen(sys->base.audioGroups)) return NULL;
    DataWin* group = sys->base.audioGroups[groupIndex];
    return group ? group : sys->base.dw;
}

static bool decodeEmbeddedSound(DataWin* dw, Sound* sound,
                                int16_t** outPcm, uint32_t* outCount,
                                int* outChannels, uint32_t* outRate) {
    if (!dw) return false;
    int32_t audioFile = sound->audioFile;
    if (audioFile < 0 || (uint32_t)audioFile >= dw->audo.count || !dw->audo.entries) return false;

    DataWin_loadAudoIfNeeded(dw, (uint32_t)audioFile);
    AudioEntry* entry = &dw->audo.entries[audioFile];
    if (!entry->data || entry->dataSize == 0) return false;
    return decodeMemoryAudio(entry->data, entry->dataSize, outPcm, outCount, outChannels, outRate);
}

static DecodedSound* findOrDecodeEmbedded(WiiAudioSystem* sys, int32_t soundIndex, Sound* sound) {
    for (int i = 0; i < (int)arrlen(sys->cache); i++) {
        if (sys->cache[i].soundIndex == soundIndex) return &sys->cache[i];
    }

    int16_t* pcm = NULL;
    uint32_t count = 0;
    int channels = 0;
    uint32_t rate = 0;
    DataWin* group = audioGroupOrNull(sys, sound->audioGroup);
    if (!group) group = sys->base.dw;
    if (!decodeEmbeddedSound(group, sound, &pcm, &count, &channels, &rate)) {
        return NULL;
    }

    DecodedSound ds;
    ds.soundIndex = soundIndex;
    ds.pcm = pcm;
    ds.sampleCount = count;
    ds.channels = channels;
    ds.sampleRate = rate;
    arrput(sys->cache, ds);
    return &sys->cache[arrlen(sys->cache) - 1];
}

static VoiceSlot* findFreeSlot(WiiAudioSystem* sys) {
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (!sys->voices[i].active) return &sys->voices[i];
    }
    // Evict lowest-priority non-streaming finished/non-looping voice if needed.
    VoiceSlot* best = NULL;
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        VoiceSlot* s = &sys->voices[i];
        if (s->streaming) continue;
        if (!s->looping) {
            if (!best || best->priority > s->priority) best = s;
        }
    }
    return best;
}

static VoiceSlot* findSlotByInstance(WiiAudioSystem* sys, int32_t instanceId) {
    if (instanceId < WII_SOUND_INSTANCE_ID_BASE) return NULL;
    int slotIndex = instanceId - WII_SOUND_INSTANCE_ID_BASE;
    if (slotIndex < 0 || slotIndex >= WII_MAX_VOICES) return NULL;
    VoiceSlot* s = &sys->voices[slotIndex];
    if (!s->active || s->instanceId != instanceId) return NULL;
    return s;
}

static VoiceSlot* findSlotBySoundOrInstance(WiiAudioSystem* sys, int32_t soundOrInstance) {
    VoiceSlot* s = findSlotByInstance(sys, soundOrInstance);
    if (s) return s;
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].soundIndex == soundOrInstance)
            return &sys->voices[i];
    }
    return NULL;
}

static void applyVoiceVolume(WiiAudioSystem* sys, VoiceSlot* slot) {
    if (!slot->voice) return;
    float effective = slot->gain * sys->masterGain;
    if (slot->muted || sys->muted || sys->suspended || slot->paused) effective = 0.0f;
    if (effective < 0.0f) effective = 0.0f;
    if (effective > 1.0f) effective = 1.0f;
    u16 vol = (u16)(effective * 255.0f);
    AESND_SetVoiceVolume(slot->voice, vol, vol);
}

static void applyVoicePitch(VoiceSlot* slot) {
    if (!slot->voice) return;
    float pitch = slot->pitch > 0.0f ? slot->pitch : 1.0f;
    AESND_SetVoiceFrequency(slot->voice, (f32)slot->sampleRate * pitch);
}

static bool fillStreamBuffer(VoiceSlot* slot, int bufIndex) {
    if (!slot->vorbis || !slot->streamBuf[bufIndex]) return false;

    int frames = stb_vorbis_get_samples_short_interleaved(
        slot->vorbis, slot->channels, slot->streamBuf[bufIndex],
        WII_STREAM_FRAMES * slot->channels);

    if (frames <= 0) {
        if (slot->looping) {
            stb_vorbis_seek_start(slot->vorbis);
            frames = stb_vorbis_get_samples_short_interleaved(
                slot->vorbis, slot->channels, slot->streamBuf[bufIndex],
                WII_STREAM_FRAMES * slot->channels);
        }
        if (frames <= 0) {
            slot->streamBufBytes[bufIndex] = 0;
            return false;
        }
    }

    uint32_t bytes = (uint32_t)frames * (uint32_t)slot->channels * sizeof(int16_t);
    slot->streamBufBytes[bufIndex] = bytes;
    flushPcm(slot->streamBuf[bufIndex], bytes);
    return true;
}

static void freeStreamResources(VoiceSlot* slot) {
    if (slot->vorbis) {
        stb_vorbis_close(slot->vorbis);
        slot->vorbis = NULL;
    }
    for (int i = 0; i < 2; i++) {
        free(slot->streamBuf[i]);
        slot->streamBuf[i] = NULL;
        slot->streamBufBytes[i] = 0;
        slot->needRefill[i] = false;
    }
    slot->streaming = false;
    slot->readyBuf = -1;
    slot->playBuf = 0;
    slot->streamEnded = false;
    slot->streamLengthSeconds = 0.0f;
}

static void stopSlot(VoiceSlot* slot) {
    if (!slot->active && !slot->voice) return;
    if (slot->voice) {
        AESND_SetVoiceStop(slot->voice, true);
        AESND_SetVoiceStream(slot->voice, false);
        AESND_FreeVoice(slot->voice);
        slot->voice = NULL;
    }
    freeStreamResources(slot);
    slot->active = false;
    slot->muted = false;
    slot->paused = false;
    slot->soundIndex = -1;
    slot->instanceId = -1;
    slot->pcm = NULL;
    slot->stoppedFlag = false;
}

static void voiceCallback(AESNDPB* pb, u32 state, void* cbArg) {
    VoiceSlot* slot = (VoiceSlot*)cbArg;
    if (!slot) return;

    if (state == VOICE_STATE_STREAM) {
        int ready = slot->readyBuf;
        if (ready >= 0 && slot->streamBufBytes[ready] > 0) {
            int old = slot->playBuf;
            slot->readyBuf = -1;
            slot->needRefill[old] = true;
            slot->playBuf = ready;
            AESND_SetVoiceBuffer(pb, slot->streamBuf[ready], slot->streamBufBytes[ready]);
        } else if (slot->streamEnded) {
            AESND_SetVoiceStream(pb, false);
            AESND_SetVoiceStop(pb, true);
            slot->stoppedFlag = true;
        } else {
            // Underrun: keep current buffer and urgently request a refill.
            int other = 1 - slot->playBuf;
            slot->needRefill[other] = true;
            if (slot->owner) slot->owner->streamUnderruns++;
        }
    } else if (state == VOICE_STATE_STOPPED) {
        slot->stoppedFlag = true;
    }
}

static bool startStreamingVoice(WiiAudioSystem* sys, VoiceSlot* slot, const char* path,
                                int32_t soundIndex, int32_t priority, bool loop,
                                float gain, float pitch) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) {
        logWarn("Audio: Failed to open '%s' (stb_vorbis err %d)\n", path, err);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(v);
    if (info.channels < 1) {
        stb_vorbis_close(v);
        return false;
    }

    int16_t* buf0 = allocAlignedPcm((uint32_t)(WII_STREAM_FRAMES * info.channels));
    int16_t* buf1 = allocAlignedPcm((uint32_t)(WII_STREAM_FRAMES * info.channels));

    slot->streaming = true;
    slot->vorbis = v;
    slot->owner = sys;
    slot->streamBuf[0] = buf0;
    slot->streamBuf[1] = buf1;
    slot->channels = info.channels;
    slot->sampleRate = (uint32_t)info.sample_rate;
    slot->looping = loop;
    slot->streamLengthSeconds = stb_vorbis_stream_length_in_seconds(v);
    slot->streamEnded = false;
    slot->stoppedFlag = false;
    slot->readyBuf = -1;
    slot->playBuf = 0;
    slot->needRefill[0] = false;
    slot->needRefill[1] = false;

    if (!fillStreamBuffer(slot, 0)) {
        freeStreamResources(slot);
        return false;
    }
    bool haveSecond = fillStreamBuffer(slot, 1);

    AESNDPB* voice = AESND_AllocateVoiceWithArg(voiceCallback, slot);
    if (!voice) {
        freeStreamResources(slot);
        return false;
    }

    u32 fmt = (info.channels >= 2) ? VOICE_STEREO16 : VOICE_MONO16;
    int slotIndex = (int)(slot - sys->voices);
    slot->soundIndex = soundIndex;
    slot->voice = voice;
    slot->pcm = NULL;
    slot->sampleCount = 0;
    slot->active = true;
    slot->muted = false;
    slot->paused = false;
    slot->gain = gain;
    slot->pitch = pitch > 0.0f ? pitch : 1.0f;
    slot->priority = priority;
    slot->instanceId = WII_SOUND_INSTANCE_ID_BASE + slotIndex;

    AESND_SetVoiceFormat(voice, fmt);
    applyVoicePitch(slot);
    applyVoiceVolume(sys, slot);
    AESND_SetVoiceStream(voice, true);
    // looped=true avoids VOICE_ONCE auto-stop so STREAM callbacks can swap buffers.
    AESND_PlayVoice(voice, fmt, slot->streamBuf[0], slot->streamBufBytes[0],
                    (f32)slot->sampleRate * slot->pitch, 0, true);

    if (haveSecond) {
        slot->readyBuf = 1;
    } else {
        slot->streamEnded = true;
    }
    return true;
}

static bool startOneShotVoice(WiiAudioSystem* sys, VoiceSlot* slot, DecodedSound* ds,
                              int32_t soundIndex, int32_t priority, bool loop,
                              float gain, float pitch) {
    AESNDPB* voice = AESND_AllocateVoiceWithArg(voiceCallback, slot);
    if (!voice) return false;

    u32 fmt = (ds->channels >= 2) ? VOICE_STEREO16 : VOICE_MONO16;
    int slotIndex = (int)(slot - sys->voices);

    slot->streaming = false;
    slot->owner = sys;
    slot->soundIndex = soundIndex;
    slot->voice = voice;
    slot->pcm = ds->pcm;
    slot->sampleCount = ds->sampleCount;
    slot->channels = ds->channels;
    slot->sampleRate = ds->sampleRate;
    slot->looping = loop;
    slot->active = true;
    slot->muted = false;
    slot->paused = false;
    slot->gain = gain;
    slot->pitch = pitch > 0.0f ? pitch : 1.0f;
    slot->priority = priority;
    slot->instanceId = WII_SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->stoppedFlag = false;
    slot->streamEnded = false;

    AESND_SetVoiceFormat(voice, fmt);
    applyVoicePitch(slot);
    applyVoiceVolume(sys, slot);
    AESND_SetVoiceStream(voice, false);
    AESND_PlayVoice(voice, fmt, ds->pcm,
                    ds->sampleCount * sizeof(int16_t),
                    (f32)ds->sampleRate * slot->pitch, 0, loop);
    return true;
}

static void wiiInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    audio->dw = dataWin;
    arrput(audio->audioGroups, dataWin);
    sys->fileSystem = fileSystem;
    AESND_Init();
}

static void wiiDestroy(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    for (int i = 0; i < WII_MAX_VOICES; i++) stopSlot(&sys->voices[i]);
    for (int i = 0; i < (int)arrlen(sys->cache); i++) free(sys->cache[i].pcm);
    arrfree(sys->cache);
    for (int i = 0; i < WII_MAX_AUDIO_STREAMS; i++) {
        free(sys->streams[i].filePath);
        sys->streams[i].filePath = NULL;
        sys->streams[i].active = false;
    }
    AESND_Reset();
    if (arrlen(sys->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t)arrlen(sys->base.audioGroups); i++) {
            DataWin_free(sys->base.audioGroups[i]);
        }
    }
    arrfree(sys->base.audioGroups);
    free(sys);
}

static void wiiUpdate(AudioSystem* audio, MAYBE_UNUSED float deltaTime) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        VoiceSlot* slot = &sys->voices[i];
        if (!slot->active) continue;

        if (slot->stoppedFlag) {
            stopSlot(slot);
            continue;
        }

        if (!slot->streaming || slot->paused) continue;

        for (int b = 0; b < 2; b++) {
            if (!slot->needRefill[b]) continue;
            if (fillStreamBuffer(slot, b)) {
                slot->needRefill[b] = false;
                if (slot->readyBuf < 0) slot->readyBuf = b;
            } else {
                slot->needRefill[b] = false;
                slot->streamEnded = true;
            }
        }
    }
}

static int32_t wiiPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (sys->muted || sys->suspended) return -1;

    bool isStream = (soundIndex >= WII_AUDIO_STREAM_INDEX_BASE);
    Sound* sound = NULL;
    const char* streamPath = NULL;
    float gain = 1.0f;
    float pitch = 1.0f;

    if (isStream) {
        int32_t streamSlot = soundIndex - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot < 0 || streamSlot >= WII_MAX_AUDIO_STREAMS || !sys->streams[streamSlot].active) {
            logWarn("Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        streamPath = sys->streams[streamSlot].filePath;
        gain = sys->streams[streamSlot].initialGain;
        pitch = sys->streams[streamSlot].initialPitch;
    } else {
        DataWin* dw = sys->base.audioGroups[0];
        if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) {
            logWarn("Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
        if (!sound->present) return -1;
        gain = sound->volume;
        pitch = sound->pitch;
    }

    VoiceSlot* slot = findFreeSlot(sys);
    if (!slot) {
        logWarn("Audio: No free voice slots for sound %d\n", soundIndex);
        return -1;
    }
    if (slot->active) stopSlot(slot);

    bool ok = false;
    if (isStream) {
        ok = startStreamingVoice(sys, slot, streamPath, soundIndex, priority, loop, gain, pitch);
    } else if (soundInAudo(sound)) {
        DecodedSound* ds = findOrDecodeEmbedded(sys, soundIndex, sound);
        if (!ds) {
            logWarn("Audio: Failed to decode embedded sound '%s'\n", sound->name ? sound->name : "?");
            return -1;
        }
        ok = startOneShotVoice(sys, slot, ds, soundIndex, priority, loop, gain, pitch);
    } else {
        char* path = resolveExternalPath(sys, sound);
        if (!path) {
            logWarn("Audio: Could not resolve path for sound '%s'\n", sound->name ? sound->name : "?");
            return -1;
        }
        ok = startStreamingVoice(sys, slot, path, soundIndex, priority, loop, gain, pitch);
        free(path);
    }

    if (!ok) return -1;
    return slot->instanceId;
}

static void wiiStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        VoiceSlot* slot = findSlotByInstance(sys, soundOrInstance);
        if (slot) stopSlot(slot);
        return;
    }
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].soundIndex == soundOrInstance) {
            stopSlot(&sys->voices[i]);
        }
    }
}

static void wiiStopAll(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    for (int i = 0; i < WII_MAX_VOICES; i++) stopSlot(&sys->voices[i]);
}

static bool wiiIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    return findSlotBySoundOrInstance(sys, soundOrInstance) != NULL;
}

static void wiiPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (!slot || !slot->voice) return;
    slot->paused = true;
    AESND_SetVoiceMute(slot->voice, true);
    applyVoiceVolume(sys, slot);
}

static void wiiResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (!slot || !slot->voice) return;
    slot->paused = false;
    AESND_SetVoiceMute(slot->voice, false);
    applyVoiceVolume(sys, slot);
}

static void wiiPauseAll(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    sys->suspended = true;
    AESND_Pause(true);

    // Drop any queued stream PCM so resume doesn't blast stale buffers that
    // piled up while the system menu held the main loop (no audio update).
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        VoiceSlot* slot = &sys->voices[i];
        if (!slot->active || !slot->streaming) continue;
        for (int b = 0; b < 2; b++) {
            if (!slot->streamBuf[b]) continue;
            uint32_t bytes = slot->streamBufBytes[b];
            if (bytes == 0) bytes = (uint32_t)WII_STREAM_FRAMES * (uint32_t)slot->channels * sizeof(int16_t);
            memset(slot->streamBuf[b], 0, bytes);
            flushPcm(slot->streamBuf[b], bytes);
            slot->streamBufBytes[b] = bytes;
            slot->needRefill[b] = true;
        }
        slot->readyBuf = -1;
        if (slot->voice && slot->streamBuf[slot->playBuf]) {
            AESND_SetVoiceBuffer(slot->voice, slot->streamBuf[slot->playBuf],
                                 slot->streamBufBytes[slot->playBuf]);
        }
    }
}

static void wiiResumeAll(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    sys->suspended = false;
    // Refill stream buffers before unpausing AESND so the first callback
    // after resume has fresh PCM instead of the silence we parked.
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        VoiceSlot* slot = &sys->voices[i];
        if (!slot->active || !slot->streaming || slot->paused) continue;
        for (int b = 0; b < 2; b++) {
            if (slot->needRefill[b] || slot->streamBufBytes[b] == 0) {
                fillStreamBuffer(slot, b);
                slot->needRefill[b] = false;
            }
        }
        if (slot->readyBuf < 0) {
            int other = 1 - slot->playBuf;
            if (slot->streamBufBytes[other] > 0) slot->readyBuf = other;
        }
        if (slot->voice && slot->streamBuf[slot->playBuf] && slot->streamBufBytes[slot->playBuf] > 0) {
            AESND_SetVoiceBuffer(slot->voice, slot->streamBuf[slot->playBuf],
                                 slot->streamBufBytes[slot->playBuf]);
        }
        applyVoiceVolume(sys, slot);
    }
    AESND_Pause(false);
}

static void wiiSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, MAYBE_UNUSED uint32_t timeMs) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (gain < 0.0f) gain = 0.0f;

    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot >= 0 && streamSlot < WII_MAX_AUDIO_STREAMS && sys->streams[streamSlot].active) {
            sys->streams[streamSlot].initialGain = gain;
        }
    }

    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        VoiceSlot* slot = findSlotByInstance(sys, soundOrInstance);
        if (slot) {
            slot->gain = gain;
            applyVoiceVolume(sys, slot);
        }
        return;
    }

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].soundIndex == soundOrInstance) {
            sys->voices[i].gain = gain;
            applyVoiceVolume(sys, &sys->voices[i]);
        }
    }
}

static float wiiGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot >= 0 && streamSlot < WII_MAX_AUDIO_STREAMS && sys->streams[streamSlot].active) {
            return sys->streams[streamSlot].initialGain;
        }
    }
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    return slot ? slot->gain : 1.0f;
}

static void wiiSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (pitch <= 0.0f) pitch = 1.0f;

    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot >= 0 && streamSlot < WII_MAX_AUDIO_STREAMS && sys->streams[streamSlot].active) {
            sys->streams[streamSlot].initialPitch = pitch;
        }
    }

    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        VoiceSlot* slot = findSlotByInstance(sys, soundOrInstance);
        if (slot) {
            slot->pitch = pitch;
            applyVoicePitch(slot);
        }
        return;
    }

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].soundIndex == soundOrInstance) {
            sys->voices[i].pitch = pitch;
            applyVoicePitch(&sys->voices[i]);
        }
    }
}

static float wiiGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot >= 0 && streamSlot < WII_MAX_AUDIO_STREAMS && sys->streams[streamSlot].active) {
            return sys->streams[streamSlot].initialPitch;
        }
    }
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    return slot ? slot->pitch : 1.0f;
}

static float wiiGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 0.0f;
}

static void wiiSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float positionSeconds) {}

static float wiiGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;

    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (slot) {
        if (slot->streaming) return slot->streamLengthSeconds;
        int channels = slot->channels > 0 ? slot->channels : 1;
        return (float)((double)(slot->sampleCount / (uint32_t)channels) / (double)slot->sampleRate);
    }

    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot < 0 || streamSlot >= WII_MAX_AUDIO_STREAMS || !sys->streams[streamSlot].active) return 0.0f;
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(sys->streams[streamSlot].filePath, &err, NULL);
        if (!v) return 0.0f;
        float seconds = stb_vorbis_stream_length_in_seconds(v);
        stb_vorbis_close(v);
        return seconds;
    }

    if (soundOrInstance < 0) return 0.0f;
    DataWin* dw = sys->base.audioGroups[0];
    if ((uint32_t)soundOrInstance >= dw->sond.count) return 0.0f;
    Sound* sound = &dw->sond.sounds[soundOrInstance];

    for (int i = 0; i < (int)arrlen(sys->cache); i++) {
        if (sys->cache[i].soundIndex == soundOrInstance) {
            int ch = sys->cache[i].channels > 0 ? sys->cache[i].channels : 1;
            return (float)((double)(sys->cache[i].sampleCount / (uint32_t)ch) / (double)sys->cache[i].sampleRate);
        }
    }

    if (soundInAudo(sound)) {
        DecodedSound* ds = findOrDecodeEmbedded(sys, soundOrInstance, sound);
        if (!ds) return 0.0f;
        int ch = ds->channels > 0 ? ds->channels : 1;
        return (float)((double)(ds->sampleCount / (uint32_t)ch) / (double)ds->sampleRate);
    }

    char* path = resolveExternalPath(sys, sound);
    if (!path) return 0.0f;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
    free(path);
    if (!v) return 0.0f;
    float seconds = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    return seconds;
}

static void wiiSetMasterGain(AudioSystem* audio, float gain) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    sys->masterGain = gain;
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active) applyVoiceVolume(sys, &sys->voices[i]);
    }
}

static void wiiSetMasterGainForListener(AudioSystem* audio, float gain, MAYBE_UNUSED int32_t listenerId) {
    wiiSetMasterGain(audio, gain);
}

static void wiiSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {}

static void wiiGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (groupIndex <= 0) return;
    if (arrlen(audio->audioGroups) > groupIndex) return;

    if (!audio->dw || audio->dw->agrp.count <= (uint32_t)groupIndex) {
        logWarn("Audio: Wanted to load Audio Group %d, but it does not exist in AGRP!\n", groupIndex);
        return;
    }

    AudioGroup* audioGroupEntry = &audio->dw->agrp.audioGroups[groupIndex];
    char* buf;
    if (audioGroupEntry->path == nullptr) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        buf = (char*)safeMalloc((size_t)sz + 1);
        snprintf(buf, (size_t)sz + 1, "audiogroup%d.dat", groupIndex);
    } else {
        buf = safeStrdup(audioGroupEntry->path);
    }

    FileSystem* fileSystem = sys->fileSystem;
    if (!fileSystem || !fileSystem->vtable->fileExists(fileSystem, buf)) {
        logWarn("Audio: Wanted to load Audio Group %d, but the audiogroup file does not exist!\n", groupIndex);
        free(buf);
        arrput(audio->audioGroups, (DataWin*)safeCalloc(1, sizeof(DataWin)));
        return;
    }

    char* resolved = fileSystem->vtable->resolvePath(fileSystem, buf);
    free(buf);
    if (!resolved) {
        arrput(audio->audioGroups, (DataWin*)safeCalloc(1, sizeof(DataWin)));
        return;
    }

    DataWinParserOptions options = {0};
    options.parseAudo = true;
    options.lazyLoadAudio = audio->dw->lazyLoadAudio;
    DataWin* audioGroup = DataWin_parse(resolved, options);
    free(resolved);
    arrput(audio->audioGroups, audioGroup);
}

static bool wiiGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return arrlen(audio->audioGroups) > groupIndex;
}

static int32_t wiiCreateStream(AudioSystem* audio, const char* filename) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (!sys->fileSystem || !filename) return -1;

    int32_t freeSlot = -1;
    for (int i = 0; i < WII_MAX_AUDIO_STREAMS; i++) {
        if (!sys->streams[i].active) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot < 0) {
        logWarn("Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
    if (!resolved) {
        logWarn("Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    sys->streams[freeSlot].active = true;
    sys->streams[freeSlot].filePath = resolved;
    sys->streams[freeSlot].initialGain = 1.0f;
    sys->streams[freeSlot].initialPitch = 1.0f;
    return WII_AUDIO_STREAM_INDEX_BASE + freeSlot;
}

static bool wiiDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    int32_t slotIndex = streamIndex - WII_AUDIO_STREAM_INDEX_BASE;
    if (slotIndex < 0 || slotIndex >= WII_MAX_AUDIO_STREAMS) return false;

    WiiAudioStreamEntry* entry = &sys->streams[slotIndex];
    if (!entry->active) return false;

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].soundIndex == streamIndex) {
            stopSlot(&sys->voices[i]);
        }
    }

    free(entry->filePath);
    entry->filePath = NULL;
    entry->active = false;
    return true;
}

static AudioSystemVtable wiiVtable;

WiiAudioSystem* WiiAudioSystem_create(void) {
    WiiAudioSystem* sys = (WiiAudioSystem*)safeCalloc(1, sizeof(WiiAudioSystem));

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        sys->voices[i].active = false;
        sys->voices[i].soundIndex = -1;
        sys->voices[i].instanceId = -1;
        sys->voices[i].voice = NULL;
        sys->voices[i].readyBuf = -1;
        sys->voices[i].pitch = 1.0f;
        sys->voices[i].gain = 1.0f;
    }

    sys->masterGain = 1.0f;
    sys->muted = false;
    sys->suspended = false;
    sys->cache = NULL;
    sys->fileSystem = NULL;

    wiiVtable.init = wiiInit;
    wiiVtable.destroy = wiiDestroy;
    wiiVtable.update = wiiUpdate;
    wiiVtable.playSound = wiiPlaySound;
    wiiVtable.stopSound = wiiStopSound;
    wiiVtable.stopAll = wiiStopAll;
    wiiVtable.isPlaying = wiiIsPlaying;
    wiiVtable.pauseSound = wiiPauseSound;
    wiiVtable.resumeSound = wiiResumeSound;
    wiiVtable.pauseAll = wiiPauseAll;
    wiiVtable.resumeAll = wiiResumeAll;
    wiiVtable.suspend = wiiPauseAll;
    wiiVtable.resume = wiiResumeAll;
    wiiVtable.setSoundGain = wiiSetSoundGain;
    wiiVtable.getSoundGain = wiiGetSoundGain;
    wiiVtable.setSoundPitch = wiiSetSoundPitch;
    wiiVtable.getSoundPitch = wiiGetSoundPitch;
    wiiVtable.getTrackPosition = wiiGetTrackPosition;
    wiiVtable.setTrackPosition = wiiSetTrackPosition;
    wiiVtable.getSoundLength = wiiGetSoundLength;
    wiiVtable.setMasterGain = wiiSetMasterGain;
    wiiVtable.setMasterGainForListener = wiiSetMasterGainForListener;
    wiiVtable.setChannelCount = wiiSetChannelCount;
    wiiVtable.groupLoad = wiiGroupLoad;
    wiiVtable.groupIsLoaded = wiiGroupIsLoaded;
    wiiVtable.createStream = wiiCreateStream;
    wiiVtable.destroyStream = wiiDestroyStream;

    sys->base.vtable = &wiiVtable;
    return sys;
}

void WiiAudioSystem_queryStats(AudioSystem* audio, WiiAudioStats* out) {
    if (!out) return;
    out->activeVoices = 0;
    out->streamingVoices = 0;
    out->cachedSounds = 0;
    out->cachedPcmBytes = 0;
    out->streamUnderruns = 0;
    if (!audio) return;

    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (!sys->voices[i].active) continue;
        out->activeVoices++;
        if (sys->voices[i].streaming) out->streamingVoices++;
    }
    out->cachedSounds = (int)arrlen(sys->cache);
    for (int i = 0; i < (int)arrlen(sys->cache); i++) {
        out->cachedPcmBytes += (size_t)sys->cache[i].sampleCount * sizeof(int16_t);
    }
    out->streamUnderruns = sys->streamUnderruns;
}
