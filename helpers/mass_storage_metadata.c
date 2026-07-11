#include "mass_storage_metadata.h"

#include <stddef.h>

#define MASS_STORAGE_METADATA_VERSION (1UL)

#define MASS_STORAGE_METADATA_FLAG_MOUNT_VALID   (1UL << 0)
#define MASS_STORAGE_METADATA_FLAG_OPTICAL_VALID (1UL << 1)
#define MASS_STORAGE_METADATA_FLAGS_MASK \
    (MASS_STORAGE_METADATA_FLAG_MOUNT_VALID | MASS_STORAGE_METADATA_FLAG_OPTICAL_VALID)

#define MASS_STORAGE_OPTICAL_FLAG_OPEN      (1UL << 0)
#define MASS_STORAGE_OPTICAL_FLAG_FINALIZED (1UL << 1)
#define MASS_STORAGE_OPTICAL_FLAG_FORMATTED (1UL << 2)
#define MASS_STORAGE_OPTICAL_FLAGS_MASK                                     \
    (MASS_STORAGE_OPTICAL_FLAG_OPEN | MASS_STORAGE_OPTICAL_FLAG_FINALIZED | \
     MASS_STORAGE_OPTICAL_FLAG_FORMATTED)

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t flags;
    uint32_t mount_read_only     : 1;
    uint32_t mount_reserved      : 7;
    uint32_t mount_exit_on_eject : 8;
    uint32_t mount_device_type   : 8;
    uint32_t mount_audio_output  : 8;
    uint32_t image_size_low;
    uint32_t image_size_high;
    uint32_t image_fingerprint_low;
    uint32_t image_fingerprint_high;
    uint32_t next_writable_lba;
    uint32_t formatted_blocks;
    uint32_t packet_size;
    uint32_t optical_flags;
    uint32_t checksum;
} MassStorageMetadataRecord;

_Static_assert(sizeof(MassStorageMetadataRecord) == 60, "metadata record size changed");

static const uint8_t mass_storage_metadata_magic[8] = {'F', 'Z', 'M', 'S', 'M', 'E', 'T', 'A'};

static uint32_t mass_storage_metadata_checksum(const MassStorageMetadataRecord* record) {
    const uint8_t* bytes = (const uint8_t*)record;
    uint32_t checksum = 2166136261UL;
    for(size_t i = 0; i < offsetof(MassStorageMetadataRecord, checksum); i++) {
        checksum ^= bytes[i];
        checksum *= 16777619UL;
    }
    return checksum;
}

static void mass_storage_metadata_path(FuriString* path, const char* image_path, bool temporary) {
    furi_string_printf(
        path, "%s%s%s", image_path, MASS_STORAGE_METADATA_SUFFIX, temporary ? ".tmp" : "");
}

static bool mass_storage_metadata_remove_path(Storage* storage, const char* path) {
    return !storage_file_exists(storage, path) || storage_common_remove(storage, path) == FSE_OK;
}

bool mass_storage_metadata_load(
    Storage* storage,
    const char* image_path,
    MassStorageMetadata* metadata) {
    memset(metadata, 0, sizeof(MassStorageMetadata));

    FuriString* path = furi_string_alloc();
    mass_storage_metadata_path(path, image_path, false);
    File* file = storage_file_alloc(storage);
    MassStorageMetadataRecord record;
    bool success =
        storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING) &&
        storage_file_size(file) == sizeof(record) &&
        storage_file_read(file, &record, sizeof(record)) == sizeof(record);
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);

    if(!success || memcmp(record.magic, mass_storage_metadata_magic, sizeof(record.magic)) ||
       record.version != MASS_STORAGE_METADATA_VERSION || record.record_size != sizeof(record) ||
       (record.flags & ~MASS_STORAGE_METADATA_FLAGS_MASK) ||
       (record.optical_flags & ~MASS_STORAGE_OPTICAL_FLAGS_MASK) ||
       record.checksum != mass_storage_metadata_checksum(&record)) {
        return false;
    }

    metadata->mount.valid = record.flags & MASS_STORAGE_METADATA_FLAG_MOUNT_VALID;
    metadata->mount.read_only = record.mount_read_only;
    metadata->mount.exit_on_eject = record.mount_exit_on_eject;
    metadata->mount.device_type = record.mount_device_type;
    metadata->mount.audio_output = record.mount_audio_output;
    metadata->optical_state_valid = record.flags & MASS_STORAGE_METADATA_FLAG_OPTICAL_VALID;
    metadata->image_size = (uint64_t)record.image_size_high << 32 | record.image_size_low;
    metadata->image_fingerprint = (uint64_t)record.image_fingerprint_high << 32 |
                                  record.image_fingerprint_low;
    metadata->optical_state.next_writable_lba = record.next_writable_lba;
    metadata->optical_state.formatted_blocks = record.formatted_blocks;
    metadata->optical_state.packet_size = record.packet_size;
    metadata->optical_state.open = record.optical_flags & MASS_STORAGE_OPTICAL_FLAG_OPEN;
    metadata->optical_state.finalized = record.optical_flags & MASS_STORAGE_OPTICAL_FLAG_FINALIZED;
    metadata->optical_state.formatted = record.optical_flags & MASS_STORAGE_OPTICAL_FLAG_FORMATTED;
    return true;
}

bool mass_storage_metadata_save(
    Storage* storage,
    const char* image_path,
    const MassStorageMetadata* metadata) {
    MassStorageMetadataRecord record = {0};
    memcpy(record.magic, mass_storage_metadata_magic, sizeof(record.magic));
    record.version = MASS_STORAGE_METADATA_VERSION;
    record.record_size = sizeof(record);
    if(metadata->mount.valid) record.flags |= MASS_STORAGE_METADATA_FLAG_MOUNT_VALID;
    if(metadata->optical_state_valid) record.flags |= MASS_STORAGE_METADATA_FLAG_OPTICAL_VALID;
    record.mount_read_only = metadata->mount.read_only;
    record.mount_exit_on_eject = metadata->mount.exit_on_eject;
    record.mount_device_type = metadata->mount.device_type;
    record.mount_audio_output = metadata->mount.audio_output;
    record.image_size_low = (uint32_t)metadata->image_size;
    record.image_size_high = metadata->image_size >> 32;
    record.image_fingerprint_low = (uint32_t)metadata->image_fingerprint;
    record.image_fingerprint_high = metadata->image_fingerprint >> 32;
    record.next_writable_lba = metadata->optical_state.next_writable_lba;
    record.formatted_blocks = metadata->optical_state.formatted_blocks;
    record.packet_size = metadata->optical_state.packet_size;
    if(metadata->optical_state.open) record.optical_flags |= MASS_STORAGE_OPTICAL_FLAG_OPEN;
    if(metadata->optical_state.finalized) {
        record.optical_flags |= MASS_STORAGE_OPTICAL_FLAG_FINALIZED;
    }
    if(metadata->optical_state.formatted) {
        record.optical_flags |= MASS_STORAGE_OPTICAL_FLAG_FORMATTED;
    }
    record.checksum = mass_storage_metadata_checksum(&record);

    FuriString* path = furi_string_alloc();
    FuriString* temporary_path = furi_string_alloc();
    mass_storage_metadata_path(path, image_path, false);
    mass_storage_metadata_path(temporary_path, image_path, true);

    File* file = storage_file_alloc(storage);
    bool success =
        storage_file_open(
            file, furi_string_get_cstr(temporary_path), FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
        storage_file_write(file, &record, sizeof(record)) == sizeof(record) &&
        storage_file_sync(file);
    storage_file_close(file);
    storage_file_free(file);

    if(success) {
        success = storage_common_rename(
                      storage, furi_string_get_cstr(temporary_path), furi_string_get_cstr(path)) ==
                  FSE_OK;
    }
    if(!success) {
        mass_storage_metadata_remove_path(storage, furi_string_get_cstr(temporary_path));
    }

    furi_string_free(temporary_path);
    furi_string_free(path);
    return success;
}

void mass_storage_metadata_remove(Storage* storage, const char* image_path) {
    FuriString* path = furi_string_alloc();
    mass_storage_metadata_path(path, image_path, false);
    mass_storage_metadata_remove_path(storage, furi_string_get_cstr(path));
    mass_storage_metadata_path(path, image_path, true);
    mass_storage_metadata_remove_path(storage, furi_string_get_cstr(path));
    furi_string_free(path);
}
