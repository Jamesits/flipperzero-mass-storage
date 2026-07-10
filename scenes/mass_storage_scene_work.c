#include "../mass_storage_app_i.h"
#include "../views/mass_storage_view.h"
#include "../helpers/mass_storage_usb.h"
#include <lib/toolbox/path.h>

#define TAG "MassStorageSceneWork"

static uint32_t mass_storage_block_size(MassStorageApp* app) {
    return app->device_type == MassStorageDeviceTypeOptical ? 2048 : SCSI_BLOCK_SIZE;
}

static bool file_prepare_part(
    MassStorageApp* app,
    uint64_t offset,
    File** file,
    uint8_t* part,
    uint64_t* part_offset) {
    uint64_t requested_part = app->file_count > 1 ? offset / MASS_STORAGE_FILE_PART_SIZE : 0;
    if(requested_part >= app->file_count) return false;

    *part = requested_part;
    *file = app->files[*part];
    *part_offset = app->file_count > 1 ? offset % MASS_STORAGE_FILE_PART_SIZE : offset;
    if(*part_offset >= app->file_sizes[*part]) return false;

    if(app->file_offsets[*part] != *part_offset) {
        if(!mass_storage_file_seek(*file, *part_offset)) return false;
        app->file_offsets[*part] = *part_offset;
    }
    return true;
}

static bool file_read(
    void* ctx,
    uint32_t lba,
    uint32_t count,
    uint8_t* out,
    uint32_t* out_len,
    uint32_t out_cap) {
    MassStorageApp* app = ctx;
    uint32_t block_size = mass_storage_block_size(app);
    FURI_LOG_T(TAG, "file_read lba=%08lX count=%08lX out_cap=%08lX", lba, count, out_cap);
    uint64_t requested = (uint64_t)count * block_size;
    uint32_t remaining = requested < out_cap ? requested : out_cap;
    uint64_t offset = (uint64_t)lba * block_size;
    *out_len = 0;

    while(remaining > 0) {
        File* file;
        uint8_t part;
        uint64_t part_offset;
        if(!file_prepare_part(app, offset, &file, &part, &part_offset)) return false;

        uint32_t chunk = MIN(remaining, app->file_sizes[part] - part_offset);
        uint32_t bytes_read = storage_file_read(file, out + *out_len, chunk);
        app->file_offsets[part] += bytes_read;
        *out_len += bytes_read;
        offset += bytes_read;
        remaining -= bytes_read;
        if(bytes_read != chunk || chunk == 0) return false;
    }

    FURI_LOG_T(TAG, "%lu/%llu", *out_len, requested);
    app->bytes_read += *out_len;
    return true;
}

static bool file_write(void* ctx, uint32_t lba, uint16_t count, uint8_t* buf, uint32_t len) {
    MassStorageApp* app = ctx;
    uint32_t block_size = mass_storage_block_size(app);
    FURI_LOG_T(TAG, "file_write lba=%08lX count=%04X len=%08lX", lba, count, len);
    if(app->read_only || app->device_type == MassStorageDeviceTypeOptical) return false;
    if(len != count * block_size) {
        FURI_LOG_W(TAG, "bad write params count=%u len=%lu", count, len);
        return false;
    }

    uint32_t remaining = len;
    uint64_t offset = (uint64_t)lba * block_size;
    while(remaining > 0) {
        File* file;
        uint8_t part;
        uint64_t part_offset;
        if(!file_prepare_part(app, offset, &file, &part, &part_offset)) return false;

        uint32_t chunk = MIN(remaining, app->file_sizes[part] - part_offset);
        uint32_t bytes_written = storage_file_write(file, buf + len - remaining, chunk);
        app->file_offsets[part] += bytes_written;
        offset += bytes_written;
        remaining -= bytes_written;
        if(bytes_written != chunk || chunk == 0) return false;
    }

    app->bytes_written += len;
    return true;
}

static uint32_t file_num_blocks(void* ctx) {
    MassStorageApp* app = ctx;
    uint64_t size = 0;
    for(uint8_t part = 0; part < app->file_count; part++) {
        size += app->file_sizes[part];
    }
    return size / mass_storage_block_size(app);
}

static void file_eject(void* ctx) {
    MassStorageApp* app = ctx;
    FURI_LOG_D(TAG, "EJECT");
    if(app->exit_on_eject) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventEject);
    }
}

bool mass_storage_scene_work_on_event(void* context, SceneManagerEvent event) {
    MassStorageApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MassStorageCustomEventEject) {
            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, MassStorageSceneSettings);
            if(!consumed) {
                consumed = scene_manager_search_and_switch_to_previous_scene(
                    app->scene_manager, MassStorageSceneStart);
            }
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        mass_storage_set_stats(app->mass_storage_view, app->bytes_read, app->bytes_written);
        if(app->bytes_read != app->led_bytes_read ||
           app->bytes_written != app->led_bytes_written) {
            if(!app->led_blinking) {
                notification_message(app->notifications, &sequence_blink_start_red);
                app->led_blinking = true;
            }
            app->led_bytes_read = app->bytes_read;
            app->led_bytes_written = app->bytes_written;
        } else if(app->led_blinking) {
            notification_message(app->notifications, &sequence_blink_stop);
            app->led_blinking = false;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, MassStorageSceneSettings);
        if(!consumed) {
            consumed = scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, MassStorageSceneStart);
        }
    }
    return consumed;
}

void mass_storage_scene_work_on_enter(void* context) {
    MassStorageApp* app = context;
    app->bytes_read = app->bytes_written = 0;
    app->led_bytes_read = app->led_bytes_written = 0;
    app->led_blinking = false;

    if(!storage_file_exists(app->fs_api, furi_string_get_cstr(app->file_path))) {
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, MassStorageSceneStart);
        return;
    }

    mass_storage_app_show_loading_popup(app, true);

    app->usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    FuriString* file_name = furi_string_alloc();
    path_extract_filename(app->file_path, file_name, true);

    mass_storage_set_file_name(app->mass_storage_view, file_name);
    app->file_count = 0;
    bool read_only = app->read_only || app->device_type == MassStorageDeviceTypeOptical;
    FuriString* part_path = furi_string_alloc();
    for(uint8_t part = 0; part < MASS_STORAGE_MAX_FILE_PARTS; part++) {
        if(part == 0) {
            furi_string_set(part_path, app->file_path);
        } else {
            if(app->file_sizes[part - 1] != MASS_STORAGE_FILE_PART_SIZE) break;
            furi_string_printf(part_path, "%s.%u", furi_string_get_cstr(app->file_path), part);
            if(!storage_file_exists(app->fs_api, furi_string_get_cstr(part_path))) break;
        }

        File* file = storage_file_alloc(app->fs_api);
        furi_assert(storage_file_open(
            file,
            furi_string_get_cstr(part_path),
            read_only ? FSAM_READ : FSAM_READ | FSAM_WRITE,
            FSOM_OPEN_EXISTING));
        app->files[app->file_count] = file;
        app->file_sizes[app->file_count] = storage_file_size(file);
        app->file_offsets[app->file_count] = UINT64_MAX;
        app->file_count++;
    }
    furi_string_free(part_path);

    SCSIDeviceFunc fn = {
        .ctx = app,
        .read = file_read,
        .write = file_write,
        .num_blocks = file_num_blocks,
        .eject = file_eject,
        .read_only = read_only,
        .device_type = app->device_type,
        .block_size = mass_storage_block_size(app),
    };

    app->usb = mass_storage_usb_start(furi_string_get_cstr(file_name), fn);

    furi_string_free(file_name);

    mass_storage_app_show_loading_popup(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, MassStorageAppViewWork);
}

void mass_storage_scene_work_on_exit(void* context) {
    MassStorageApp* app = context;
    mass_storage_app_show_loading_popup(app, true);

    if(app->led_blinking) {
        notification_message(app->notifications, &sequence_blink_stop);
        app->led_blinking = false;
    }

    if(app->usb_mutex) {
        furi_mutex_free(app->usb_mutex);
        app->usb_mutex = NULL;
    }
    if(app->usb) {
        mass_storage_usb_stop(app->usb);
        app->usb = NULL;
    }
    for(uint8_t part = 0; part < app->file_count; part++) {
        storage_file_free(app->files[part]);
        app->files[part] = NULL;
    }
    app->file_count = 0;
    mass_storage_app_show_loading_popup(app, false);
}
