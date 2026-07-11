#pragma once

#include "mass_storage_scsi.h"

#include <storage/storage.h>

#define AUDIO_CD_SECTOR_SIZE     (2352UL)
#define AUDIO_CD_SECTORS_PER_SEC (75UL)
#define AUDIO_CD_SAMPLE_RATE     (44100UL)
#define AUDIO_CD_MAX_TRACKS      (99U)

typedef struct AudioCd AudioCd;

AudioCd* audio_cd_alloc(Storage* storage, const char* cue_path, FuriString* error);
void audio_cd_free(AudioCd* cd);

uint32_t audio_cd_num_sectors(const AudioCd* cd);
uint8_t audio_cd_track_count(const AudioCd* cd);
bool audio_cd_track_info(const AudioCd* cd, uint8_t track, SCSIAudioTrackInfo* info);
bool audio_cd_get_status(AudioCd* cd, SCSIAudioStatus* status);
bool audio_cd_control(AudioCd* cd, SCSIAudioControl control, uint32_t start_lba, uint32_t end_lba);

bool audio_cd_read(
    AudioCd* cd,
    uint32_t lba,
    uint32_t count,
    uint8_t* output,
    uint32_t* output_len,
    uint32_t output_capacity);
