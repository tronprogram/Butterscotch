#include "wii_audio_system.h"
#include "data_win.h"
#include "utils.h"
#include "common.h"
#include "binary_utils.h"

#include <aesndlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "stb_ds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define WII_MAX_VOICES 16

typedef struct {
    int32_t soundIndex;
    AESNDPB* voice;
    int16_t* pcm;
    uint32_t sampleCount;
    int channels;
    uint32_t sampleRate;
    bool looping;
    bool active;
    bool muted;
    float gain;
    int32_t instanceId;
} VoiceSlot;

typedef struct {
    int32_t soundIndex;
    int16_t* pcm;
    uint32_t sampleCount;
    int channels;
    uint32_t sampleRate;
} DecodedSound;

struct WiiAudioSystem {
    AudioSystem base;
    VoiceSlot voices[WII_MAX_VOICES];
    DecodedSound* cache;
    int32_t nextInstanceId;
    float masterGain;
    bool muted;
    bool suspended;
};

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
    int16_t* pcm = (int16_t*)safeMalloc(sizeof(int16_t) * sampleCount);
    memcpy(pcm, pcmData, sizeof(int16_t) * sampleCount);

#if defined(IS_BIG_ENDIAN)
    for (uint32_t i = 0; i < sampleCount; i++) {
        pcm[i] = (int16_t)BinaryUtils_bswap16((uint16_t)pcm[i]);
    }
#endif

    *outPcm = pcm;
    *outCount = sampleCount;
    *outChannels = (int)channels;
    *outRate = sampleRate;
    return true;
}

static bool decodeSound(DataWin* dw, int32_t soundIndex,
                        int16_t** outPcm, uint32_t* outCount,
                        int* outChannels, uint32_t* outRate) {
    if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return false;
    Sound* sond = &dw->sond.sounds[soundIndex];
    if (!sond->present) return false;

    int32_t audioFile = sond->audioFile;
    if (audioFile < 0 || (uint32_t)audioFile >= dw->audo.count) return false;

    DataWin_loadAudoIfNeeded(dw, (uint32_t)audioFile);
    AudioEntry* entry = &dw->audo.entries[audioFile];
    if (!entry->data || entry->dataSize == 0) return false;

    const uint8_t* data = entry->data;
    uint32_t size = entry->dataSize;

    if (size >= 4 && memcmp(data, "RIFF", 4) == 0) {
        return parseWav(data, size, outPcm, outCount, outChannels, outRate);
    }

    int channels = 0;
    int sampleRate = 0;
    int16_t* pcm = NULL;
    int samplesDecoded = stb_vorbis_decode_memory(data, (int)size, &channels, &sampleRate, &pcm);
    if (samplesDecoded <= 0 || pcm == NULL) return false;

    *outPcm = pcm;
    *outCount = (uint32_t)(samplesDecoded * channels);
    *outChannels = channels;
    *outRate = (uint32_t)sampleRate;
    return true;
}

static DecodedSound* findOrDecode(WiiAudioSystem* sys, int32_t soundIndex) {
    for (int i = 0; i < (int)arrlen(sys->cache); i++) {
        if (sys->cache[i].soundIndex == soundIndex) return &sys->cache[i];
    }

    int16_t* pcm = NULL;
    uint32_t count = 0;
    int channels = 0;
    uint32_t rate = 0;
    if (!decodeSound(sys->base.dw, soundIndex, &pcm, &count, &channels, &rate)) {
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
    return NULL;
}

static VoiceSlot* findSlotByInstance(WiiAudioSystem* sys, int32_t instanceId) {
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active && sys->voices[i].instanceId == instanceId)
            return &sys->voices[i];
    }
    return NULL;
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
    if (slot->muted || sys->muted || sys->suspended) effective = 0.0f;
    if (effective < 0.0f) effective = 0.0f;
    if (effective > 1.0f) effective = 1.0f;
    u16 vol = (u16)(effective * 255.0f);
    AESND_SetVoiceVolume(slot->voice, vol, vol);
}

static void stopSlot(VoiceSlot* slot) {
    if (!slot->active) return;
    if (slot->voice) {
        AESND_SetVoiceStop(slot->voice, true);
        AESND_FreeVoice(slot->voice);
        slot->voice = NULL;
    }
    slot->active = false;
    slot->muted = false;
    slot->soundIndex = -1;
    slot->instanceId = -1;
}

static void voiceStoppedCallback(AESNDPB* pb, u32 state) {
    // Cleared via update when voice is stopped; keep callback for API completeness.
    (void)pb;
    (void)state;
}

static void wiiInit(AudioSystem* audio, DataWin* dataWin, MAYBE_UNUSED FileSystem* fileSystem) {
    audio->dw = dataWin;
    arrput(audio->audioGroups, dataWin);
    AESND_Init();
}

static void wiiDestroy(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    for (int i = 0; i < WII_MAX_VOICES; i++) stopSlot(&sys->voices[i]);
    for (int i = 0; i < (int)arrlen(sys->cache); i++) free(sys->cache[i].pcm);
    arrfree(sys->cache);
    AESND_Reset();
    free(sys);
}

static void wiiUpdate(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED float deltaTime) {
    (void)audio;
    (void)deltaTime;
}

static int32_t wiiPlaySound(AudioSystem* audio, int32_t soundIndex, MAYBE_UNUSED int32_t priority, bool loop) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    if (sys->muted || sys->suspended) return -1;

    DecodedSound* ds = findOrDecode(sys, soundIndex);
    if (!ds) return -1;

    VoiceSlot* slot = findFreeSlot(sys);
    if (!slot) return -1;

    AESNDPB* voice = AESND_AllocateVoice(voiceStoppedCallback);
    if (!voice) return -1;

    u32 fmt = (ds->channels >= 2) ? VOICE_STEREO16 : VOICE_MONO16;
    AESND_SetVoiceFormat(voice, fmt);
    AESND_SetVoiceFrequency(voice, (f32)ds->sampleRate);
    AESND_SetVoiceLoop(voice, loop);

    int32_t iid = sys->nextInstanceId++;
    slot->soundIndex = soundIndex;
    slot->voice = voice;
    slot->pcm = ds->pcm;
    slot->sampleCount = ds->sampleCount;
    slot->channels = ds->channels;
    slot->sampleRate = ds->sampleRate;
    slot->looping = loop;
    slot->active = true;
    slot->muted = false;
    slot->gain = 1.0f;
    slot->instanceId = iid;
    applyVoiceVolume(sys, slot);

    AESND_PlayVoice(voice, fmt, ds->pcm,
                    ds->sampleCount * sizeof(int16_t),
                    (f32)ds->sampleRate, 0, loop);

    return iid;
}

static void wiiStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (slot) stopSlot(slot);
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
    if (!slot) return;
    slot->muted = true;
    applyVoiceVolume(sys, slot);
}

static void wiiResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (!slot) return;
    slot->muted = false;
    applyVoiceVolume(sys, slot);
}

static void wiiPauseAll(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    sys->suspended = true;
    AESND_Pause(true);
}

static void wiiResumeAll(AudioSystem* audio) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    sys->suspended = false;
    AESND_Pause(false);
    for (int i = 0; i < WII_MAX_VOICES; i++) {
        if (sys->voices[i].active) applyVoiceVolume(sys, &sys->voices[i]);
    }
}

static void wiiSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, MAYBE_UNUSED uint32_t timeMs) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (!slot) return;
    slot->gain = gain;
    applyVoiceVolume(sys, slot);
}

static float wiiGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    return slot ? slot->gain : 1.0f;
}

static void wiiSetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float pitch) {}

static float wiiGetSoundPitch(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 1.0f;
}

static float wiiGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 0.0f;
}

static void wiiSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float positionSeconds) {}

static float wiiGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* sys = (WiiAudioSystem*)audio;
    VoiceSlot* slot = findSlotBySoundOrInstance(sys, soundOrInstance);
    if (slot) {
        int channels = slot->channels > 0 ? slot->channels : 1;
        return (float)((double)(slot->sampleCount / (uint32_t)channels) / (double)slot->sampleRate);
    }
    for (int i = 0; i < (int)arrlen(sys->cache); i++) {
        if (sys->cache[i].soundIndex == soundOrInstance) {
            int ch = sys->cache[i].channels > 0 ? sys->cache[i].channels : 1;
            return (float)((double)(sys->cache[i].sampleCount / (uint32_t)ch) / (double)sys->cache[i].sampleRate);
        }
    }
    return 1.0f;
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
static void wiiGroupLoad(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {}
static bool wiiGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) { return true; }
static int32_t wiiCreateStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED const char* filename) { return -1; }
static bool wiiDestroyStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t streamIndex) { return false; }

static AudioSystemVtable wiiVtable;

WiiAudioSystem* WiiAudioSystem_create(void) {
    WiiAudioSystem* sys = (WiiAudioSystem*)safeCalloc(1, sizeof(WiiAudioSystem));

    for (int i = 0; i < WII_MAX_VOICES; i++) {
        sys->voices[i].active = false;
        sys->voices[i].soundIndex = -1;
        sys->voices[i].instanceId = -1;
        sys->voices[i].voice = NULL;
    }

    sys->nextInstanceId = 1;
    sys->masterGain = 1.0f;
    sys->muted = false;
    sys->suspended = false;
    sys->cache = NULL;

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
