#pragma once

#include "../helpers/mass_storage_scsi.h"

#include <gui/view.h>

typedef struct MassStorage MassStorage;

typedef enum {
    MassStorageInputPlayPause,
    MassStorageInputStop,
    MassStorageInputPrevious,
    MassStorageInputNext,
    MassStorageInputScanBackward,
    MassStorageInputScanForward,
    MassStorageInputScanEnd,
} MassStorageInput;

typedef void (*MassStorageInputCallback)(MassStorageInput input, void* context);

MassStorage* mass_storage_alloc();

void mass_storage_free(MassStorage* mass_storage);

View* mass_storage_get_view(MassStorage* mass_storage);

void mass_storage_set_file_name(MassStorage* mass_storage, FuriString* name);

void mass_storage_set_stats(MassStorage* mass_storage, uint32_t read, uint32_t written);

void mass_storage_set_wipe_progress(MassStorage* mass_storage, uint16_t progress, bool active);

void mass_storage_set_audio_mode(MassStorage* mass_storage, bool enabled);

void mass_storage_set_audio_status(
    MassStorage* mass_storage,
    const SCSIAudioStatus* status,
    uint8_t track_count,
    uint32_t track_start,
    uint32_t track_end);

void mass_storage_set_input_callback(
    MassStorage* mass_storage,
    MassStorageInputCallback callback,
    void* context);
