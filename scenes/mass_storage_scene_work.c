#include "../mass_storage_app_i.h"
#include "../views/mass_storage_view.h"
#include "../helpers/mass_storage_usb.h"
#include <lib/toolbox/path.h>

#define TAG "MassStorageSceneWork"

#define AUDIO_CD_PREVIOUS_WINDOW_MS (1500UL)

static uint32_t mass_storage_block_size(MassStorageApp* app) {
    return app->device_type == MassStorageDeviceTypeOptical ? 2048 : SCSI_BLOCK_SIZE;
}

static uint32_t mass_storage_file_block_size(MassStorageApp* app) {
    return app->audio_cd ? AUDIO_CD_SECTOR_SIZE : mass_storage_block_size(app);
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

static bool file_has_udf_volume_recognition_sequence(MassStorageApp* app) {
    bool beginning_found = false;
    uint8_t identifier[5];

    // ECMA-167 places the Volume Recognition Sequence at sector 16 or later. UDF uses
    // BEA01 followed by NSR02/NSR03 and terminates the sequence with TEA01.
    for(uint32_t lba = 16; lba < 32; lba++) {
        File* file;
        uint8_t part;
        uint64_t part_offset;
        uint64_t offset = (uint64_t)lba * 2048 + 1;
        if(!file_prepare_part(app, offset, &file, &part, &part_offset) ||
           app->file_sizes[part] - part_offset < sizeof(identifier)) {
            return false;
        }

        uint32_t bytes_read = storage_file_read(file, identifier, sizeof(identifier));
        app->file_offsets[part] += bytes_read;
        if(bytes_read != sizeof(identifier)) return false;

        if(!memcmp(identifier, "BEA01", sizeof(identifier))) {
            beginning_found = true;
        } else if(
            beginning_found && (!memcmp(identifier, "NSR02", sizeof(identifier)) ||
                                !memcmp(identifier, "NSR03", sizeof(identifier)))) {
            return true;
        } else if(beginning_found && !memcmp(identifier, "TEA01", sizeof(identifier))) {
            return false;
        }
    }

    return false;
}

static bool file_read(
    void* ctx,
    uint32_t lba,
    uint32_t count,
    uint8_t* out,
    uint32_t* out_len,
    uint32_t out_cap) {
    MassStorageApp* app = ctx;
    if(app->audio_cd) {
        bool result = audio_cd_read(app->audio_cd, lba, count, out, out_len, out_cap);
        if(result) app->bytes_read += *out_len;
        return result;
    }

    uint32_t block_size = mass_storage_file_block_size(app);
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
    uint32_t block_size = mass_storage_file_block_size(app);
    FURI_LOG_T(TAG, "file_write lba=%08lX count=%04X len=%08lX", lba, count, len);
    if(app->read_only) return false;
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

static bool file_sync(void* ctx) {
    MassStorageApp* app = ctx;
    if(app->audio_cd) return true;
    bool result = true;
    for(uint8_t part = 0; part < app->file_count; part++) {
        result = storage_file_sync(app->files[part]) && result;
    }
    return result;
}

static void file_wipe_progress(void* ctx, uint16_t progress, bool active) {
    MassStorageApp* app = ctx;
    app->wipe_progress = progress;
    app->wipe_active = active;
}

static uint32_t file_num_blocks(void* ctx) {
    MassStorageApp* app = ctx;
    if(app->audio_cd) return audio_cd_num_sectors(app->audio_cd);
    uint64_t size = 0;
    for(uint8_t part = 0; part < app->file_count; part++) {
        size += app->file_sizes[part];
    }
    return size / mass_storage_file_block_size(app);
}

static uint8_t file_audio_track_count(void* ctx) {
    MassStorageApp* app = ctx;
    return app->audio_cd ? audio_cd_track_count(app->audio_cd) : 0;
}

static bool file_audio_track_info(void* ctx, uint8_t track, SCSIAudioTrackInfo* info) {
    MassStorageApp* app = ctx;
    return app->audio_cd && audio_cd_track_info(app->audio_cd, track, info);
}

static bool file_audio_get_status(void* ctx, SCSIAudioStatus* status) {
    MassStorageApp* app = ctx;
    return app->audio_cd && audio_cd_get_status(app->audio_cd, status);
}

static bool
    file_audio_control(void* ctx, SCSIAudioControl control, uint32_t start_lba, uint32_t end_lba) {
    MassStorageApp* app = ctx;
    return app->audio_cd && audio_cd_control(app->audio_cd, control, start_lba, end_lba);
}

static void file_eject(void* ctx) {
    MassStorageApp* app = ctx;
    FURI_LOG_D(TAG, "EJECT");
    if(app->exit_on_eject != MassStorageExitOnEjectOff) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventEject);
    }
}

static void file_removed(void* ctx) {
    MassStorageApp* app = ctx;
    FURI_LOG_D(TAG, "USB REMOVED");
    if(app->exit_on_eject == MassStorageExitOnEjectUsb) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventEject);
    }
}

static void file_suspended(void* ctx) {
    MassStorageApp* app = ctx;
    FURI_LOG_D(TAG, "USB SUSPEND");
    // Suspend approximates a physical port disconnect; exit only in USB mode.
    if(app->exit_on_eject == MassStorageExitOnEjectUsb) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventEject);
    }
}

static void mass_storage_update_audio_view(MassStorageApp* app) {
    if(!app->audio_cd) return;
    SCSIAudioStatus status;
    SCSIAudioTrackInfo track;
    if(audio_cd_get_status(app->audio_cd, &status) &&
       audio_cd_track_info(app->audio_cd, status.track, &track)) {
        app->audio_selected_track = status.track;
        mass_storage_set_audio_status(
            app->mass_storage_view,
            &status,
            audio_cd_track_count(app->audio_cd),
            track.start_lba,
            track.end_lba);
    }
}

static void mass_storage_audio_input(MassStorageInput input, void* context) {
    MassStorageApp* app = context;
    if(!app->audio_cd) return;

    SCSIAudioStatus status;
    if(!audio_cd_get_status(app->audio_cd, &status)) return;
    uint32_t sectors = audio_cd_num_sectors(app->audio_cd);

    if(input == MassStorageInputPlayPause) {
        app->audio_left_tick = 0;
        if(status.status == SCSIAudioStatusPlaying) {
            audio_cd_control(app->audio_cd, SCSIAudioControlPause, 0, 0);
        } else if(status.status == SCSIAudioStatusPaused) {
            audio_cd_control(app->audio_cd, SCSIAudioControlResume, 0, 0);
        } else {
            SCSIAudioTrackInfo track;
            if(audio_cd_track_info(app->audio_cd, app->audio_selected_track, &track)) {
                audio_cd_control(app->audio_cd, SCSIAudioControlPlay, track.start_lba, sectors);
            }
        }
    } else if(input == MassStorageInputStop) {
        app->audio_left_tick = 0;
        audio_cd_control(app->audio_cd, SCSIAudioControlStop, 0, 0);
    } else if(input == MassStorageInputPrevious) {
        uint32_t now = furi_get_tick();
        uint8_t target = app->audio_selected_track;
        if(app->audio_left_tick &&
           now - app->audio_left_tick <= furi_ms_to_ticks(AUDIO_CD_PREVIOUS_WINDOW_MS)) {
            target = app->audio_left_track > 1 ? app->audio_left_track - 1 : 1;
        }
        app->audio_left_track = target;
        app->audio_selected_track = target;
        app->audio_left_tick = now;
        SCSIAudioTrackInfo track;
        if(audio_cd_track_info(app->audio_cd, target, &track)) {
            audio_cd_control(app->audio_cd, SCSIAudioControlSeek, track.start_lba, sectors);
        }
    } else if(input == MassStorageInputNext) {
        app->audio_left_tick = 0;
        uint8_t target = app->audio_selected_track + 1;
        SCSIAudioTrackInfo track;
        if(audio_cd_track_info(app->audio_cd, target, &track)) {
            app->audio_selected_track = target;
            audio_cd_control(app->audio_cd, SCSIAudioControlSeek, track.start_lba, sectors);
        }
    } else if(input == MassStorageInputScanBackward || input == MassStorageInputScanForward) {
        app->audio_left_tick = 0;
        uint32_t start = MIN(status.lba, sectors - 1);
        audio_cd_control(
            app->audio_cd,
            input == MassStorageInputScanBackward ? SCSIAudioControlScanBackward :
                                                    SCSIAudioControlScanForward,
            start,
            sectors);
    } else if(input == MassStorageInputScanEnd) {
        audio_cd_control(app->audio_cd, SCSIAudioControlScanEnd, 0, 0);
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
        if(app->audio_cd) {
            mass_storage_update_audio_view(app);
        } else {
            mass_storage_set_stats(app->mass_storage_view, app->bytes_read, app->bytes_written);
            mass_storage_set_wipe_progress(
                app->mass_storage_view, app->wipe_progress, app->wipe_active);
        }
        if(app->bytes_read != app->led_bytes_read ||
           app->bytes_written != app->led_bytes_written) {
            if(!app->led_blinking) {
                notification_message(app->notifications, &sequence_blink_start_red);
                app->led_blinking = true;
            }
            app->led_bytes_read = app->bytes_read;
            app->led_bytes_written = app->bytes_written;
        } else if(app->led_blinking) {
            // Idle: stop the read/write blink and show a steady blue.
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_set_only_blue_255);
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
    bool is_audio_cue = furi_string_end_withi(app->file_path, MASS_STORAGE_CUE_EXTENSION);
    if(furi_string_end_withi(app->file_path, MASS_STORAGE_ISO_EXTENSION)) {
        app->read_only = true;
        app->device_type = MassStorageDeviceTypeOptical;
    }
    if(is_audio_cue) {
        app->read_only = true;
        app->device_type = MassStorageDeviceTypeOptical;
    }
    app->audio_cd = NULL;
    app->audio_left_tick = 0;
    app->audio_left_track = 1;
    app->audio_selected_track = 1;
    mass_storage_set_audio_mode(app->mass_storage_view, false);
    mass_storage_set_input_callback(app->mass_storage_view, NULL, NULL);
    app->bytes_read = app->bytes_written = 0;
    app->led_bytes_read = app->led_bytes_written = 0;
    app->wipe_progress = 0;
    app->led_blinking = false;
    app->wipe_active = false;
    mass_storage_set_wipe_progress(app->mass_storage_view, 0, false);

    if(!storage_file_exists(app->fs_api, furi_string_get_cstr(app->file_path))) {
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, MassStorageSceneStart);
        return;
    }

    mass_storage_app_show_loading_popup(app, true);

    if(is_audio_cue) {
        FuriString* error = furi_string_alloc();
        FURI_LOG_I(TAG, "Loading audio CUE");
        app->audio_cd = audio_cd_alloc(app->fs_api, furi_string_get_cstr(app->file_path), error);
        if(!app->audio_cd) {
            mass_storage_app_show_loading_popup(app, false);
            dialog_message_show_storage_error(app->dialogs, furi_string_get_cstr(error));
            furi_string_free(error);
            scene_manager_previous_scene(app->scene_manager);
            return;
        }
        furi_string_free(error);
    }

    app->usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    FuriString* file_name = furi_string_alloc();
    path_extract_filename(app->file_path, file_name, true);

    mass_storage_set_file_name(app->mass_storage_view, file_name);
    app->file_count = 0;
    bool read_only = app->read_only;
    if(!app->audio_cd) {
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
    }

    bool optical_formatted = !app->audio_cd && app->device_type == MassStorageDeviceTypeOptical &&
                             !read_only && file_has_udf_volume_recognition_sequence(app);
    if(optical_formatted) {
        FURI_LOG_I(TAG, "restored formatted optical state from UDF image");
    }

    SCSIDeviceFunc fn = {
        .ctx = app,
        .read = file_read,
        .write = file_write,
        .num_blocks = file_num_blocks,
        .sync = file_sync,
        .eject = file_eject,
        .wipe_progress = file_wipe_progress,
        .removed = file_removed,
        .suspended = file_suspended,
        .audio_track_count = file_audio_track_count,
        .audio_track_info = file_audio_track_info,
        .audio_get_status = file_audio_get_status,
        .audio_control = file_audio_control,
        .read_only = read_only,
        // Removable media is a prerequisite for the host to send an eject command.
        .removable = app->audio_cd || app->exit_on_eject != MassStorageExitOnEjectOff,
        .device_type = app->device_type,
        .block_size = mass_storage_block_size(app),
        .audio_cd = app->audio_cd != NULL,
        .optical_formatted = optical_formatted,
    };

    FURI_LOG_I(TAG, "Starting USB storage");
    app->usb = mass_storage_usb_start(furi_string_get_cstr(file_name), fn);

    if(!app->usb) {
        furi_string_free(file_name);
        mass_storage_app_show_loading_popup(app, false);
        dialog_message_show_storage_error(app->dialogs, "Cannot start USB storage");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    FURI_LOG_I(TAG, "USB storage started");

    furi_string_free(file_name);

    // Disk enabled but idle: steady blue, no flashing.
    notification_message(app->notifications, &sequence_set_only_blue_255);

    if(app->audio_cd) {
        mass_storage_set_audio_mode(app->mass_storage_view, true);
        mass_storage_set_input_callback(app->mass_storage_view, mass_storage_audio_input, app);
        mass_storage_update_audio_view(app);
    }

    mass_storage_app_show_loading_popup(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, MassStorageAppViewWork);
}

void mass_storage_scene_work_on_exit(void* context) {
    MassStorageApp* app = context;
    mass_storage_app_show_loading_popup(app, true);
    mass_storage_set_input_callback(app->mass_storage_view, NULL, NULL);

    if(app->led_blinking) {
        notification_message(app->notifications, &sequence_blink_stop);
        app->led_blinking = false;
    }
    // Clear the steady blue (or any leftover color) shown while enabled.
    notification_message(app->notifications, &sequence_reset_rgb);

    if(app->usb) {
        mass_storage_usb_stop(app->usb);
        app->usb = NULL;
    }
    if(app->usb_mutex) {
        furi_mutex_free(app->usb_mutex);
        app->usb_mutex = NULL;
    }
    if(app->audio_cd) {
        audio_cd_free(app->audio_cd);
        app->audio_cd = NULL;
    }
    for(uint8_t part = 0; part < app->file_count; part++) {
        storage_file_close(app->files[part]);
        storage_file_free(app->files[part]);
        app->files[part] = NULL;
    }
    app->file_count = 0;
    mass_storage_set_audio_mode(app->mass_storage_view, false);
    mass_storage_app_show_loading_popup(app, false);
}
