#include "audio_cd.h"
#include "audio_cd_hal.h"

#include <furi_hal.h>
#include <stm32wbxx_ll_dma.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define TAG "AudioCd"

#define AUDIO_CD_MAX_CUE_SIZE        (32UL * 1024UL)
#define AUDIO_CD_DMA_HALF_SAMPLES    (2048UL)
#define AUDIO_CD_DMA_SAMPLES         (AUDIO_CD_DMA_HALF_SAMPLES * 2)
#define AUDIO_CD_RAW_HALF_SIZE       (AUDIO_CD_DMA_HALF_SAMPLES * 4)
#define AUDIO_CD_SCAN_MULTIPLIER     (4UL)
#define AUDIO_CD_PLAYER_QUEUE_LENGTH (16UL)
#define AUDIO_CD_PLAYER_STACK_SIZE   (4UL * 1024UL)

typedef struct {
    uint32_t file_index0;
    uint32_t file_index1;
    uint32_t pregap;
    uint32_t disc_index0;
    uint32_t disc_index1;
    bool has_file_index0;
    bool has_file_index1;
    bool has_pregap;
} AudioCdTrack;

typedef enum {
    AudioCdEventDmaHalf,
    AudioCdEventDmaFull,
    AudioCdEventControl,
    AudioCdEventExit,
} AudioCdEventType;

typedef struct {
    AudioCdEventType type;
    uint32_t generation;
    SCSIAudioControl control;
    uint32_t start_lba;
    uint32_t end_lba;
} AudioCdEvent;

struct AudioCd {
    Storage* storage;
    File* host_file;
    FuriString* bin_path;

    AudioCdTrack tracks[AUDIO_CD_MAX_TRACKS];
    uint8_t track_count;
    uint32_t first_file_sector;
    uint32_t file_sectors;
    uint32_t sectors;

    FuriMessageQueue* queue;
    FuriMutex* state_mutex;
    FuriMutex* file_mutex;
    FuriThread* thread;
    AudioCdOutput output;
    bool thread_started;
    bool output_ready;
    volatile uint32_t generation;

    uint8_t* dma_buffer;
    uint8_t* raw_buffer;
    uint64_t source_offset;
    uint64_t range_start_offset;
    uint64_t range_end_offset;
    uint64_t half_start[2];
    uint32_t half_valid[2];
    bool scan_active;
    bool scan_reverse;
    SCSIAudioStatusCode scan_restore_status;
    SCSIAudioStatus status;
    uint8_t volume;
};

static void audio_cd_error(FuriString* error, const char* text) {
    if(error) furi_string_set_str(error, text);
}

static char* audio_cd_trim(char* text) {
    while(isspace((unsigned char)*text))
        text++;
    char* end = text + strlen(text);
    while(end > text && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return text;
}

static bool audio_cd_keyword(const char* line, const char* keyword, const char** rest) {
    size_t length = strlen(keyword);
    if(strncasecmp(line, keyword, length) != 0 ||
       (line[length] != '\0' && !isspace((unsigned char)line[length]))) {
        return false;
    }
    line += length;
    while(isspace((unsigned char)*line))
        line++;
    *rest = line;
    return true;
}

static bool audio_cd_parse_number(const char** text, uint32_t* value) {
    char* end;
    unsigned long parsed = strtoul(*text, &end, 10);
    if(end == *text || parsed > UINT32_MAX) return false;
    *value = parsed;
    while(isspace((unsigned char)*end))
        end++;
    *text = end;
    return true;
}

static bool audio_cd_parse_msf(const char* text, uint32_t* sectors) {
    uint32_t minute;
    uint32_t second;
    uint32_t frame;
    if(!audio_cd_parse_number(&text, &minute) || *text++ != ':' ||
       !audio_cd_parse_number(&text, &second) || *text++ != ':' ||
       !audio_cd_parse_number(&text, &frame) || *text != '\0' || second >= 60 ||
       frame >= AUDIO_CD_SECTORS_PER_SEC) {
        return false;
    }
    uint64_t result = ((uint64_t)minute * 60 + second) * AUDIO_CD_SECTORS_PER_SEC + frame;
    if(result > UINT32_MAX) return false;
    *sectors = result;
    return true;
}

static bool audio_cd_parse_file_name(const char* text, FuriString* name) {
    const char* start = text;
    const char* end;
    if(*start == '"') {
        start++;
        end = strchr(start, '"');
        if(!end) return false;
        text = end + 1;
    } else {
        end = start;
        while(*end && !isspace((unsigned char)*end))
            end++;
        text = end;
    }
    while(isspace((unsigned char)*text))
        text++;
    if(end == start || strcasecmp(text, "BINARY") != 0) return false;
    furi_string_set_strn(name, start, end - start);
    return true;
}

static void
    audio_cd_resolve_bin_path(AudioCd* cd, const char* cue_path, const FuriString* file_name) {
    FuriString* normalized = furi_string_alloc();
    const char* source = furi_string_get_cstr(file_name);
    for(size_t i = 0; source[i]; i++) {
        furi_string_push_back(normalized, source[i] == '\\' ? '/' : source[i]);
    }

    source = furi_string_get_cstr(normalized);
    if(source[0] == '/') {
        furi_string_set(cd->bin_path, normalized);
    } else {
        const char* slash = strrchr(cue_path, '/');
        if(slash) {
            furi_string_set_strn(cd->bin_path, cue_path, slash - cue_path);
            furi_string_cat_str(cd->bin_path, "/");
        } else {
            furi_string_reset(cd->bin_path);
        }
        furi_string_cat(cd->bin_path, normalized);
    }
    furi_string_free(normalized);
}

static bool audio_cd_parse_cue(AudioCd* cd, const char* cue_path, FuriString* error) {
    File* cue_file = storage_file_alloc(cd->storage);
    bool opened = storage_file_open(cue_file, cue_path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(!opened) {
        storage_file_close(cue_file);
        storage_file_free(cue_file);
        audio_cd_error(error, "Cannot open CUE file");
        return false;
    }

    uint64_t cue_size = storage_file_size(cue_file);
    if(cue_size == 0 || cue_size > AUDIO_CD_MAX_CUE_SIZE) {
        storage_file_close(cue_file);
        storage_file_free(cue_file);
        audio_cd_error(error, "CUE file is empty\nor too large");
        return false;
    }

    char* buffer = malloc(cue_size + 1);
    if(!buffer) {
        storage_file_close(cue_file);
        storage_file_free(cue_file);
        audio_cd_error(error, "Not enough memory\nto parse CUE");
        return false;
    }
    size_t bytes_read = storage_file_read(cue_file, buffer, cue_size);
    storage_file_close(cue_file);
    storage_file_free(cue_file);
    if(bytes_read != cue_size || memchr(buffer, '\0', cue_size)) {
        free(buffer);
        audio_cd_error(error, "Cannot read CUE file");
        return false;
    }
    buffer[cue_size] = '\0';

    FuriString* file_name = furi_string_alloc();
    bool file_seen = false;
    bool valid = true;
    int16_t current_track = -1;
    uint32_t line_number = 0;
    char* cursor = buffer;
    while(valid && *cursor) {
        char* line = cursor;
        char* newline = strchr(cursor, '\n');
        if(newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        line_number++;
        line = audio_cd_trim(line);
        if(line_number == 1 && strlen(line) >= 3 && (uint8_t)line[0] == 0xEF &&
           (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) {
            line += 3;
        }
        if(*line == '\0') continue;

        const char* rest;
        if(audio_cd_keyword(line, "REM", &rest) || audio_cd_keyword(line, "TITLE", &rest) ||
           audio_cd_keyword(line, "PERFORMER", &rest) ||
           audio_cd_keyword(line, "SONGWRITER", &rest) ||
           audio_cd_keyword(line, "CATALOG", &rest) || audio_cd_keyword(line, "ISRC", &rest) ||
           audio_cd_keyword(line, "FLAGS", &rest)) {
            continue;
        } else if(audio_cd_keyword(line, "FILE", &rest)) {
            if(file_seen || !audio_cd_parse_file_name(rest, file_name)) {
                valid = false;
                audio_cd_error(error, "Use one FILE ... BINARY\nin the CUE sheet");
            } else {
                file_seen = true;
            }
        } else if(audio_cd_keyword(line, "TRACK", &rest)) {
            uint32_t number;
            if(!file_seen || !audio_cd_parse_number(&rest, &number) ||
               number != (uint32_t)cd->track_count + 1 || number > AUDIO_CD_MAX_TRACKS ||
               strcasecmp(rest, "AUDIO") != 0) {
                valid = false;
                audio_cd_error(error, "Only sequential AUDIO\ntracks are supported");
            } else {
                current_track = cd->track_count++;
            }
        } else if(audio_cd_keyword(line, "INDEX", &rest)) {
            uint32_t index;
            uint32_t sector;
            if(current_track < 0 || !audio_cd_parse_number(&rest, &index) ||
               !audio_cd_parse_msf(rest, &sector)) {
                valid = false;
                audio_cd_error(error, "Invalid INDEX in CUE");
            } else if(index == 0) {
                AudioCdTrack* track = &cd->tracks[current_track];
                if(track->has_file_index0 || track->has_pregap) {
                    valid = false;
                    audio_cd_error(error, "Invalid track pregap\nin CUE");
                } else {
                    track->file_index0 = sector;
                    track->has_file_index0 = true;
                }
            } else if(index == 1) {
                AudioCdTrack* track = &cd->tracks[current_track];
                if(track->has_file_index1) {
                    valid = false;
                    audio_cd_error(error, "Duplicate INDEX 01\nin CUE");
                } else {
                    track->file_index1 = sector;
                    track->has_file_index1 = true;
                }
            }
        } else if(audio_cd_keyword(line, "PREGAP", &rest)) {
            AudioCdTrack* track = current_track >= 0 ? &cd->tracks[current_track] : NULL;
            uint32_t pregap;
            if(!track || track->has_pregap || track->has_file_index0 ||
               !audio_cd_parse_msf(rest, &pregap)) {
                valid = false;
                audio_cd_error(error, "Invalid PREGAP in CUE");
            } else {
                track->pregap = pregap;
                track->has_pregap = pregap != 0;
            }
        } else if(audio_cd_keyword(line, "POSTGAP", &rest)) {
            uint32_t postgap;
            if(!audio_cd_parse_msf(rest, &postgap) || postgap != 0) {
                valid = false;
                audio_cd_error(error, "POSTGAP is not supported");
            }
        }
    }

    if(valid && (!file_seen || cd->track_count == 0)) {
        valid = false;
        audio_cd_error(error, "CUE has no audio tracks");
    }
    for(uint8_t i = 0; valid && i < cd->track_count; i++) {
        AudioCdTrack* track = &cd->tracks[i];
        uint32_t boundary = track->has_file_index0 ? track->file_index0 : track->file_index1;
        if(!track->has_file_index1 ||
           (track->has_file_index0 && track->file_index0 > track->file_index1) ||
           (i > 0 && (track->file_index1 <= cd->tracks[i - 1].file_index1 ||
                      boundary <= cd->tracks[i - 1].file_index1))) {
            valid = false;
            audio_cd_error(error, "Track indexes are invalid");
        }
    }

    if(valid) audio_cd_resolve_bin_path(cd, cue_path, file_name);
    furi_string_free(file_name);
    free(buffer);
    return valid;
}

static File* audio_cd_open_bin(AudioCd* cd) {
    File* file = storage_file_alloc(cd->storage);
    if(!storage_file_open(
           file, furi_string_get_cstr(cd->bin_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        return NULL;
    }
    return file;
}

static bool audio_cd_build_layout(AudioCd* cd, FuriString* error) {
    uint64_t size = storage_file_size(cd->host_file);
    if(size == 0 || size % AUDIO_CD_SECTOR_SIZE || size / AUDIO_CD_SECTOR_SIZE > UINT32_MAX) {
        audio_cd_error(error, "BIN size is not whole\n2352-byte sectors");
        return false;
    }
    cd->file_sectors = size / AUDIO_CD_SECTOR_SIZE;
    cd->first_file_sector = cd->tracks[0].file_index1;
    if(cd->first_file_sector >= cd->file_sectors) {
        audio_cd_error(error, "First track is outside BIN");
        return false;
    }

    uint64_t inserted = 0;
    for(uint8_t i = 0; i < cd->track_count; i++) {
        AudioCdTrack* track = &cd->tracks[i];
        if(track->file_index1 >= cd->file_sectors || track->file_index1 < cd->first_file_sector ||
           (track->has_file_index0 && track->file_index0 < cd->first_file_sector)) {
            audio_cd_error(error, "Track is outside BIN file");
            return false;
        }

        uint64_t index0;
        uint64_t index1 = (uint64_t)track->file_index1 - cd->first_file_sector + inserted;
        if(track->has_pregap && i > 0) {
            index0 = index1;
            inserted += track->pregap;
            index1 += track->pregap;
        } else if(track->has_file_index0 && i > 0) {
            index0 = (uint64_t)track->file_index0 - cd->first_file_sector + inserted;
        } else {
            index0 = index1;
        }
        if(index1 > UINT32_MAX || index0 > UINT32_MAX) {
            audio_cd_error(error, "Audio CD is too large");
            return false;
        }
        track->disc_index0 = index0;
        track->disc_index1 = index1;
    }

    uint64_t sectors = (uint64_t)cd->file_sectors - cd->first_file_sector + inserted;
    if(sectors == 0 || sectors > UINT32_MAX) {
        audio_cd_error(error, "Audio CD is too large");
        return false;
    }
    cd->sectors = sectors;
    return true;
}

static bool audio_cd_locate(
    const AudioCd* cd,
    uint64_t disc_offset,
    bool* silence,
    uint64_t* file_offset,
    uint64_t* available) {
    uint64_t disc_size = (uint64_t)cd->sectors * AUDIO_CD_SECTOR_SIZE;
    if(disc_offset >= disc_size) return false;

    uint64_t inserted = 0;
    uint64_t next_boundary = disc_size;
    for(uint8_t i = 1; i < cd->track_count; i++) {
        const AudioCdTrack* track = &cd->tracks[i];
        if(!track->has_pregap) continue;
        uint64_t start = (uint64_t)track->disc_index0 * AUDIO_CD_SECTOR_SIZE;
        uint64_t end = (uint64_t)track->disc_index1 * AUDIO_CD_SECTOR_SIZE;
        if(disc_offset < start) {
            next_boundary = start;
            break;
        }
        if(disc_offset < end) {
            *silence = true;
            *file_offset = 0;
            *available = end - disc_offset;
            return true;
        }
        inserted += end - start;
    }

    *silence = false;
    *file_offset = (uint64_t)cd->first_file_sector * AUDIO_CD_SECTOR_SIZE + disc_offset - inserted;
    *available = next_boundary - disc_offset;
    return true;
}

static bool audio_cd_read_bytes(AudioCd* cd, uint64_t offset, uint8_t* output, uint32_t length) {
    if(furi_mutex_acquire(cd->file_mutex, FuriWaitForever) != FuriStatusOk) return false;

    bool result = true;
    uint32_t done = 0;
    while(done < length) {
        bool silence;
        uint64_t file_offset;
        uint64_t available;
        if(!audio_cd_locate(cd, offset + done, &silence, &file_offset, &available)) {
            result = false;
            break;
        }
        uint32_t chunk = MIN((uint64_t)(length - done), available);
        if(!chunk) {
            result = false;
            break;
        }
        if(silence) {
            memset(output + done, 0, chunk);
        } else {
            if(file_offset > UINT32_MAX ||
               !storage_file_seek(cd->host_file, (uint32_t)file_offset, true) ||
               storage_file_read(cd->host_file, output + done, chunk) != chunk) {
                result = false;
                break;
            }
        }
        done += chunk;
    }
    furi_mutex_release(cd->file_mutex);
    return result;
}

static uint8_t audio_cd_track_at_lba(const AudioCd* cd, uint32_t lba, uint8_t* index) {
    for(uint8_t i = cd->track_count; i > 0; i--) {
        const AudioCdTrack* track = &cd->tracks[i - 1];
        if(lba >= track->disc_index0) {
            *index = lba < track->disc_index1 ? 0 : 1;
            return i;
        }
    }
    *index = 1;
    return 1;
}

static void
    audio_cd_set_status(AudioCd* cd, SCSIAudioStatusCode status, uint32_t lba, uint32_t end_lba) {
    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    cd->status.status = status;
    if(status != SCSIAudioStatusPlaying) cd->status.scan_direction = SCSIAudioScanNone;
    cd->status.lba = MIN(lba, cd->sectors);
    cd->status.end_lba = MIN(end_lba, cd->sectors);
    furi_mutex_release(cd->state_mutex);
}

static void audio_cd_set_scan_direction(AudioCd* cd, SCSIAudioScanDirection direction) {
    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    cd->status.scan_direction = direction;
    furi_mutex_release(cd->state_mutex);
}

static void audio_cd_dma_isr(void* context) {
    AudioCd* cd = context;
    if(LL_DMA_IsActiveFlag_HT1(DMA1)) {
        LL_DMA_ClearFlag_HT1(DMA1);
        AudioCdEvent event = {
            .type = AudioCdEventDmaHalf,
            .generation = cd->generation,
        };
        furi_message_queue_put(cd->queue, &event, 0);
    }
    if(LL_DMA_IsActiveFlag_TC1(DMA1)) {
        LL_DMA_ClearFlag_TC1(DMA1);
        AudioCdEvent event = {
            .type = AudioCdEventDmaFull,
            .generation = cd->generation,
        };
        furi_message_queue_put(cd->queue, &event, 0);
    }
}

static bool audio_cd_fill_half(AudioCd* cd, uint8_t half) {
    uint8_t* output = cd->dma_buffer + half * AUDIO_CD_DMA_HALF_SAMPLES;
    memset(output, UINT8_MAX / 2, AUDIO_CD_DMA_HALF_SAMPLES);

    bool reverse_exhausted = cd->scan_active && cd->scan_reverse &&
                             cd->source_offset <= cd->range_start_offset;
    uint64_t start;
    if(cd->scan_active && cd->scan_reverse) {
        uint64_t step = (uint64_t)AUDIO_CD_RAW_HALF_SIZE * AUDIO_CD_SCAN_MULTIPLIER;
        start = cd->source_offset > cd->range_start_offset + step ? cd->source_offset - step :
                                                                    cd->range_start_offset;
    } else {
        start = cd->source_offset;
    }
    cd->half_start[half] = start;

    uint64_t limit = cd->range_end_offset;
    uint32_t raw_length = !reverse_exhausted && start < limit ?
                              MIN((uint64_t)AUDIO_CD_RAW_HALF_SIZE, limit - start) :
                              0;
    raw_length -= raw_length % 4;
    if(raw_length && !audio_cd_read_bytes(cd, start, cd->raw_buffer, raw_length)) {
        cd->half_valid[half] = 0;
        audio_cd_set_status(cd, SCSIAudioStatusError, start / AUDIO_CD_SECTOR_SIZE, cd->sectors);
        return false;
    }

    uint32_t samples = raw_length / 4;
    uint8_t volume = audio_cd_get_volume(cd);
    for(uint32_t i = 0; i < samples; i++) {
        const uint8_t* sample = cd->raw_buffer + i * 4;
        int16_t left = (uint16_t)sample[0] | (uint16_t)sample[1] << 8;
        int16_t right = (uint16_t)sample[2] | (uint16_t)sample[3] << 8;
        int32_t mono = left / 2 + right / 2;
        int32_t scaled = mono * (int32_t)volume / (int32_t)AUDIO_CD_VOLUME_MAX;
        output[i] = (scaled >> 8) + 128;
    }
    cd->half_valid[half] = samples;

    if(cd->scan_active && cd->scan_reverse) {
        cd->source_offset = start;
    } else if(cd->scan_active) {
        uint64_t step = (uint64_t)AUDIO_CD_RAW_HALF_SIZE * AUDIO_CD_SCAN_MULTIPLIER;
        cd->source_offset = MIN(start + step, cd->range_end_offset);
    } else {
        cd->source_offset = start + raw_length;
    }
    return true;
}

static bool
    audio_cd_begin_playback(AudioCd* cd, uint32_t start_lba, uint32_t end_lba, bool reverse) {
    furi_assert(cd->output_ready);
    audio_cd_hal_speaker_stop();
    audio_cd_hal_dma_stop();
    cd->generation++;
    cd->range_start_offset = reverse ? 0 : (uint64_t)start_lba * AUDIO_CD_SECTOR_SIZE;
    cd->range_end_offset = (uint64_t)end_lba * AUDIO_CD_SECTOR_SIZE;
    cd->source_offset = (uint64_t)start_lba * AUDIO_CD_SECTOR_SIZE;
    if(!audio_cd_fill_half(cd, 0) || !audio_cd_fill_half(cd, 1)) return false;
    if(cd->half_valid[0] == 0) {
        audio_cd_set_status(cd, SCSIAudioStatusCompleted, start_lba, end_lba);
        return true;
    }

    audio_cd_hal_dma_init((uint32_t)cd->dma_buffer, AUDIO_CD_DMA_SAMPLES);
    audio_cd_hal_dma_start();
    audio_cd_set_status(
        cd, SCSIAudioStatusPlaying, cd->half_start[0] / AUDIO_CD_SECTOR_SIZE, end_lba);
    audio_cd_hal_speaker_start();
    return true;
}

static void audio_cd_stop_output(AudioCd* cd) {
    if(cd->output_ready) {
        audio_cd_hal_speaker_stop();
        audio_cd_hal_dma_stop();
    }
    cd->generation++;
}

static void audio_cd_handle_dma(AudioCd* cd, const AudioCdEvent* event) {
    if(event->generation != cd->generation) return;

    uint8_t completed_half = event->type == AudioCdEventDmaHalf ? 0 : 1;
    uint8_t playing_half = completed_half ^ 1;
    if(cd->half_valid[playing_half] == 0) {
        audio_cd_stop_output(cd);
        uint32_t position = cd->scan_reverse ? cd->range_start_offset / AUDIO_CD_SECTOR_SIZE :
                                               cd->range_end_offset / AUDIO_CD_SECTOR_SIZE;
        audio_cd_set_status(
            cd, SCSIAudioStatusCompleted, position, cd->range_end_offset / AUDIO_CD_SECTOR_SIZE);
        return;
    }

    audio_cd_set_status(
        cd,
        SCSIAudioStatusPlaying,
        cd->half_start[playing_half] / AUDIO_CD_SECTOR_SIZE,
        cd->range_end_offset / AUDIO_CD_SECTOR_SIZE);
    if(!audio_cd_fill_half(cd, completed_half)) audio_cd_stop_output(cd);
}

static void audio_cd_handle_control(AudioCd* cd, const AudioCdEvent* event) {
    SCSIAudioStatus current;
    audio_cd_get_status(cd, &current);

    switch(event->control) {
    case SCSIAudioControlPlay:
        cd->scan_active = false;
        audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
        audio_cd_begin_playback(cd, event->start_lba, event->end_lba, false);
        break;
    case SCSIAudioControlPause:
        if(current.status == SCSIAudioStatusPlaying) {
            audio_cd_stop_output(cd);
            cd->scan_active = false;
            audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
            audio_cd_set_status(cd, SCSIAudioStatusPaused, current.lba, current.end_lba);
        }
        break;
    case SCSIAudioControlResume:
        if(current.status == SCSIAudioStatusPaused) {
            cd->scan_active = false;
            audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
            audio_cd_begin_playback(cd, current.lba, current.end_lba, false);
        }
        break;
    case SCSIAudioControlStop:
        audio_cd_stop_output(cd);
        cd->scan_active = false;
        audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
        audio_cd_set_status(
            cd, SCSIAudioStatusStopped, cd->tracks[current.track - 1].disc_index1, current.end_lba);
        break;
    case SCSIAudioControlScanForward:
    case SCSIAudioControlScanBackward:
        cd->scan_restore_status = current.status;
        cd->scan_active = true;
        cd->scan_reverse = event->control == SCSIAudioControlScanBackward;
        audio_cd_set_scan_direction(
            cd, cd->scan_reverse ? SCSIAudioScanBackward : SCSIAudioScanForward);
        audio_cd_begin_playback(cd, event->start_lba, event->end_lba, cd->scan_reverse);
        break;
    case SCSIAudioControlScanEnd:
        if(cd->scan_active) {
            audio_cd_stop_output(cd);
            cd->scan_active = false;
            audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
            if(cd->scan_restore_status == SCSIAudioStatusPlaying) {
                audio_cd_begin_playback(cd, current.lba, current.end_lba, false);
            } else {
                SCSIAudioStatusCode status = cd->scan_restore_status == SCSIAudioStatusPaused ?
                                                 SCSIAudioStatusPaused :
                                                 SCSIAudioStatusStopped;
                audio_cd_set_status(cd, status, current.lba, current.end_lba);
            }
        }
        break;
    case SCSIAudioControlSeek:
        audio_cd_stop_output(cd);
        cd->scan_active = false;
        audio_cd_set_scan_direction(cd, SCSIAudioScanNone);
        if(current.status == SCSIAudioStatusPlaying) {
            audio_cd_begin_playback(cd, event->start_lba, event->end_lba, false);
        } else {
            SCSIAudioStatusCode status = current.status == SCSIAudioStatusPaused ?
                                             SCSIAudioStatusPaused :
                                             SCSIAudioStatusStopped;
            audio_cd_set_status(cd, status, event->start_lba, event->end_lba);
        }
        break;
    }
}

static bool audio_cd_control_needs_output(AudioCd* cd, SCSIAudioControl control) {
    if(control == SCSIAudioControlPlay || control == SCSIAudioControlScanForward ||
       control == SCSIAudioControlScanBackward) {
        return true;
    }

    SCSIAudioStatus status;
    audio_cd_get_status(cd, &status);
    return (control == SCSIAudioControlResume && status.status == SCSIAudioStatusPaused) ||
           (control == SCSIAudioControlSeek && status.status == SCSIAudioStatusPlaying);
}

static bool audio_cd_prepare_output(AudioCd* cd) {
    if(cd->output_ready) return true;

    SCSIAudioStatus status;
    audio_cd_get_status(cd, &status);

    if(!cd->dma_buffer) cd->dma_buffer = malloc(AUDIO_CD_DMA_SAMPLES);
    if(!cd->raw_buffer) cd->raw_buffer = malloc(AUDIO_CD_RAW_HALF_SIZE);
    if(!cd->dma_buffer || !cd->raw_buffer) {
        FURI_LOG_E(TAG, "Cannot allocate playback resources");
        audio_cd_set_status(cd, SCSIAudioStatusError, status.lba, cd->sectors);
        return false;
    }

    if(!furi_hal_speaker_acquire(1000)) {
        FURI_LOG_W(TAG, "Speaker is busy");
        audio_cd_set_status(cd, SCSIAudioStatusError, status.lba, cd->sectors);
        return false;
    }

    memset(cd->dma_buffer, UINT8_MAX / 2, AUDIO_CD_DMA_SAMPLES);
    audio_cd_hal_init(
        AUDIO_CD_SAMPLE_RATE,
        (cd->output & AudioCdOutputSpeaker) != 0,
        (cd->output & AudioCdOutputExternal) != 0);
    audio_cd_hal_dma_stop();
    cd->output_ready = true;
    furi_hal_interrupt_set_isr(FuriHalInterruptIdDma1Ch1, audio_cd_dma_isr, cd);
    FURI_LOG_I(TAG, "Audio output ready");
    return true;
}

static int32_t audio_cd_player_worker(void* context) {
    AudioCd* cd = context;

    AudioCdEvent event;
    while(furi_message_queue_get(cd->queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == AudioCdEventExit) break;
        if(event.type == AudioCdEventControl) {
            if(audio_cd_control_needs_output(cd, event.control) && !audio_cd_prepare_output(cd)) {
                continue;
            }
            audio_cd_handle_control(cd, &event);
        } else if(cd->output_ready) {
            audio_cd_handle_dma(cd, &event);
        }
    }

    if(cd->output_ready) {
        audio_cd_hal_speaker_stop();
        audio_cd_hal_dma_stop();
        furi_hal_interrupt_set_isr(FuriHalInterruptIdDma1Ch1, NULL, NULL);
        audio_cd_hal_deinit();
        furi_hal_speaker_release();
        cd->output_ready = false;
    }
    return 0;
}

static bool audio_cd_start_player(AudioCd* cd) {
    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    if(cd->thread_started) {
        furi_mutex_release(cd->state_mutex);
        return true;
    }

    cd->queue = furi_message_queue_alloc(AUDIO_CD_PLAYER_QUEUE_LENGTH, sizeof(AudioCdEvent));
    cd->thread = furi_thread_alloc();
    if(!cd->queue || !cd->thread) {
        if(cd->thread) {
            furi_thread_free(cd->thread);
            cd->thread = NULL;
        }
        if(cd->queue) {
            furi_message_queue_free(cd->queue);
            cd->queue = NULL;
        }
        cd->status.status = SCSIAudioStatusError;
        furi_mutex_release(cd->state_mutex);
        FURI_LOG_E(TAG, "Cannot allocate playback worker");
        return false;
    }

    furi_thread_set_name(cd->thread, "AudioCdPlayer");
    furi_thread_set_stack_size(cd->thread, AUDIO_CD_PLAYER_STACK_SIZE);
    furi_thread_set_context(cd->thread, cd);
    furi_thread_set_callback(cd->thread, audio_cd_player_worker);
    cd->thread_started = true;
    furi_thread_start(cd->thread);
    furi_mutex_release(cd->state_mutex);
    FURI_LOG_I(TAG, "Playback worker started");
    return true;
}

AudioCd* audio_cd_alloc(
    Storage* storage,
    const char* cue_path,
    AudioCdOutput output,
    FuriString* error) {
    furi_assert(storage);
    furi_assert(cue_path);
    furi_assert(output < AudioCdOutputCount);

    AudioCd* cd = malloc(sizeof(AudioCd));
    if(!cd) {
        audio_cd_error(error, "Not enough memory");
        return NULL;
    }
    memset(cd, 0, sizeof(AudioCd));
    cd->storage = storage;
    cd->output = output;
    cd->bin_path = furi_string_alloc();

    FURI_LOG_I(TAG, "Parsing CUE %s", cue_path);
    if(!audio_cd_parse_cue(cd, cue_path, error)) {
        audio_cd_free(cd);
        return NULL;
    }
    cd->host_file = audio_cd_open_bin(cd);
    if(!cd->host_file) {
        audio_cd_error(error, "Cannot open companion BIN");
        audio_cd_free(cd);
        return NULL;
    }
    if(!audio_cd_build_layout(cd, error)) {
        audio_cd_free(cd);
        return NULL;
    }

    cd->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    cd->file_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!cd->state_mutex || !cd->file_mutex) {
        audio_cd_error(error, "Not enough memory");
        audio_cd_free(cd);
        return NULL;
    }
    cd->status.status = SCSIAudioStatusStopped;
    cd->status.lba = cd->tracks[0].disc_index1;
    cd->status.end_lba = cd->sectors;
    cd->status.track = 1;
    cd->status.index = 1;
    cd->volume = AUDIO_CD_VOLUME_MAX;
    FURI_LOG_I(TAG, "CUE ready: %u tracks, %lu sectors", cd->track_count, cd->sectors);
    return cd;
}

void audio_cd_free(AudioCd* cd) {
    if(!cd) return;
    if(cd->thread_started) {
        AudioCdEvent event = {.type = AudioCdEventExit};
        furi_message_queue_put(cd->queue, &event, FuriWaitForever);
        furi_thread_join(cd->thread);
    }
    if(cd->thread) furi_thread_free(cd->thread);
    if(cd->host_file) {
        storage_file_close(cd->host_file);
        storage_file_free(cd->host_file);
    }
    if(cd->state_mutex) furi_mutex_free(cd->state_mutex);
    if(cd->file_mutex) furi_mutex_free(cd->file_mutex);
    if(cd->queue) furi_message_queue_free(cd->queue);
    if(cd->bin_path) furi_string_free(cd->bin_path);
    free(cd->raw_buffer);
    free(cd->dma_buffer);
    free(cd);
}

uint32_t audio_cd_num_sectors(const AudioCd* cd) {
    furi_assert(cd);
    return cd->sectors;
}

uint8_t audio_cd_track_count(const AudioCd* cd) {
    furi_assert(cd);
    return cd->track_count;
}

bool audio_cd_track_info(const AudioCd* cd, uint8_t track, SCSIAudioTrackInfo* info) {
    furi_assert(cd);
    furi_assert(info);
    if(track == 0 || track > cd->track_count) return false;

    const AudioCdTrack* source = &cd->tracks[track - 1];
    info->number = track;
    info->start_lba = source->disc_index1;
    info->index0_lba = source->disc_index0;
    info->has_index0 = track > 1 && source->disc_index0 < source->disc_index1;
    if(track < cd->track_count) {
        info->end_lba = cd->tracks[track].disc_index0;
    } else {
        info->end_lba = cd->sectors;
    }
    return true;
}

bool audio_cd_get_status(AudioCd* cd, SCSIAudioStatus* status) {
    furi_assert(cd);
    furi_assert(status);
    if(!cd->state_mutex) return false;

    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    *status = cd->status;
    furi_mutex_release(cd->state_mutex);
    status->track = audio_cd_track_at_lba(cd, MIN(status->lba, cd->sectors - 1), &status->index);
    return true;
}

uint8_t audio_cd_get_volume(AudioCd* cd) {
    furi_assert(cd);
    furi_assert(cd->state_mutex);

    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    uint8_t volume = cd->volume;
    furi_mutex_release(cd->state_mutex);
    return volume;
}

void audio_cd_set_volume(AudioCd* cd, uint8_t volume) {
    furi_assert(cd);
    furi_assert(cd->state_mutex);

    furi_mutex_acquire(cd->state_mutex, FuriWaitForever);
    cd->volume = MIN(volume, AUDIO_CD_VOLUME_MAX);
    furi_mutex_release(cd->state_mutex);
}

bool audio_cd_control(AudioCd* cd, SCSIAudioControl control, uint32_t start_lba, uint32_t end_lba) {
    furi_assert(cd);
    if(start_lba > cd->sectors || end_lba > cd->sectors) return false;
    if((control == SCSIAudioControlPlay || control == SCSIAudioControlScanForward ||
        control == SCSIAudioControlScanBackward || control == SCSIAudioControlSeek) &&
       (start_lba >= end_lba || end_lba == 0)) {
        return false;
    }
    if(!audio_cd_start_player(cd)) return false;

    AudioCdEvent event = {
        .type = AudioCdEventControl,
        .control = control,
        .start_lba = start_lba,
        .end_lba = end_lba,
    };
    return furi_message_queue_put(cd->queue, &event, 100) == FuriStatusOk;
}

bool audio_cd_read(
    AudioCd* cd,
    uint32_t lba,
    uint32_t count,
    uint8_t* output,
    uint32_t* output_len,
    uint32_t output_capacity) {
    furi_assert(cd);
    furi_assert(output);
    furi_assert(output_len);
    *output_len = 0;
    if(lba > cd->sectors || count > cd->sectors - lba) return false;

    uint32_t blocks = MIN(count, output_capacity / AUDIO_CD_SECTOR_SIZE);
    if(count && !blocks) {
        FURI_LOG_E(TAG, "Read buffer is smaller than one raw sector");
        return false;
    }
    uint32_t length = blocks * AUDIO_CD_SECTOR_SIZE;
    if(length && !audio_cd_read_bytes(cd, (uint64_t)lba * AUDIO_CD_SECTOR_SIZE, output, length)) {
        return false;
    }
    *output_len = length;
    return true;
}
