#ifndef _BS_WII_AUDIO_SYSTEM_H_
#define _BS_WII_AUDIO_SYSTEM_H_

#include "audio_system.h"
#include <stddef.h>
#include <stdint.h>

typedef struct WiiAudioSystem WiiAudioSystem;

typedef struct {
    int activeVoices;
    int streamingVoices;
    int cachedSounds;
    size_t cachedPcmBytes;
} WiiAudioStats;

WiiAudioSystem* WiiAudioSystem_create(void);
void WiiAudioSystem_queryStats(AudioSystem* audio, WiiAudioStats* out);

#endif /* _BS_WII_AUDIO_SYSTEM_H_ */
