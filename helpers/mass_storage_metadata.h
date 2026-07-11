#pragma once

#include "mass_storage_scsi.h"

#include <storage/storage.h>

#define MASS_STORAGE_METADATA_SUFFIX ".msmeta"

typedef struct {
    bool valid;
    bool read_only;
    uint8_t exit_on_eject;
    uint8_t device_type;
    uint8_t audio_output;
} MassStorageMountOptions;

typedef struct {
    MassStorageMountOptions mount;
    bool optical_state_valid;
    uint64_t image_size;
    uint64_t image_fingerprint;
    SCSIOpticalState optical_state;
} MassStorageMetadata;

bool mass_storage_metadata_load(
    Storage* storage,
    const char* image_path,
    MassStorageMetadata* metadata);
bool mass_storage_metadata_save(
    Storage* storage,
    const char* image_path,
    const MassStorageMetadata* metadata);
void mass_storage_metadata_remove(Storage* storage, const char* image_path);
