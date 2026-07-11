#include "mass_storage_view.h"
#include "../mass_storage_app_i.h"
#include <gui/elements.h>

struct MassStorage {
    View* view;
    MassStorageInputCallback callback;
    void* context;
};

typedef struct {
    FuriString *file_name, *status_string;
    uint32_t read_speed, write_speed;
    uint32_t bytes_read, bytes_written;
    uint32_t update_time;
    uint16_t wipe_progress;
    bool wipe_active;
    bool audio_mode;
    SCSIAudioStatus audio_status;
    uint8_t track_count;
    uint32_t track_start;
    uint32_t track_end;
    uint8_t volume;
} MassStorageModel;

static const char* mass_storage_audio_status_name(const SCSIAudioStatus* status) {
    switch(status->status) {
    case SCSIAudioStatusPlaying:
        if(status->scan_direction == SCSIAudioScanForward) return "FF";
        if(status->scan_direction == SCSIAudioScanBackward) return "Rewind";
        return "Playing";
    case SCSIAudioStatusPaused:
        return "Paused";
    case SCSIAudioStatusCompleted:
        return "Complete";
    case SCSIAudioStatusError:
        return "Audio error";
    case SCSIAudioStatusStopped:
        return "Stopped";
    case SCSIAudioStatusNone:
    default:
        return "Ready";
    }
}

static void mass_storage_draw_audio(Canvas* canvas, MassStorageModel* model) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, canvas_width(canvas) / 2, 0, AlignCenter, AlignTop, "Audio CD");

    canvas_set_font(canvas, FontSecondary);
    elements_string_fit_width(canvas, model->file_name, 116);
    canvas_draw_str_aligned(
        canvas, 64, 12, AlignCenter, AlignTop, furi_string_get_cstr(model->file_name));

    const uint8_t volume_height = 29;
    uint8_t volume_fill = model->volume * volume_height / AUDIO_CD_VOLUME_MAX;
    canvas_draw_frame(canvas, 124, 12, 4, volume_height + 2);
    if(volume_fill) canvas_draw_box(canvas, 125, 13 + volume_height - volume_fill, 2, volume_fill);

    furi_string_printf(
        model->status_string,
        "Track %u/%u  %s",
        model->audio_status.track,
        model->track_count,
        mass_storage_audio_status_name(&model->audio_status));
    canvas_draw_str_aligned(
        canvas, 64, 24, AlignCenter, AlignTop, furi_string_get_cstr(model->status_string));

    uint32_t position = CLAMP(model->audio_status.lba, model->track_end, model->track_start);
    uint32_t elapsed = (position - model->track_start) / AUDIO_CD_SECTORS_PER_SEC;
    uint32_t duration = (model->track_end - model->track_start) / AUDIO_CD_SECTORS_PER_SEC;
    furi_string_printf(
        model->status_string,
        "%02lu:%02lu / %02lu:%02lu",
        elapsed / 60,
        elapsed % 60,
        duration / 60,
        duration % 60);
    canvas_draw_str_aligned(
        canvas, 64, 35, AlignCenter, AlignTop, furi_string_get_cstr(model->status_string));

    uint32_t range = model->track_end - model->track_start;
    uint32_t progress = range ? (position - model->track_start) * 108 / range : 0;
    canvas_draw_frame(canvas, 9, 46, 110, 5);
    if(progress) canvas_draw_box(canvas, 10, 47, progress, 3);

    // Previous track
    canvas_draw_line(canvas, 20, 55, 20, 63);
    canvas_draw_line(canvas, 21, 59, 27, 55);
    canvas_draw_line(canvas, 21, 59, 27, 63);
    canvas_draw_line(canvas, 28, 59, 34, 55);
    canvas_draw_line(canvas, 28, 59, 34, 63);

    // Play/pause
    if(model->audio_status.status == SCSIAudioStatusPlaying) {
        canvas_draw_box(canvas, 60, 55, 3, 9);
        canvas_draw_box(canvas, 66, 55, 3, 9);
    } else {
        canvas_draw_line(canvas, 60, 55, 60, 63);
        canvas_draw_line(canvas, 60, 55, 68, 59);
        canvas_draw_line(canvas, 60, 63, 68, 59);
    }

    // Next track
    canvas_draw_line(canvas, 108, 55, 108, 63);
    canvas_draw_line(canvas, 107, 59, 101, 55);
    canvas_draw_line(canvas, 107, 59, 101, 63);
    canvas_draw_line(canvas, 100, 59, 94, 55);
    canvas_draw_line(canvas, 100, 59, 94, 63);
}

static void append_suffixed_byte_count(FuriString* string, uint32_t count) {
    if(count < 1024) {
        furi_string_cat_printf(string, "%luB", count);
    } else if(count < 1024 * 1024) {
        furi_string_cat_printf(string, "%luK", count / 1024);
    } else if(count < 1024 * 1024 * 1024) {
        furi_string_cat_printf(string, "%.3fM", (double)count / (1024 * 1024));
    } else {
        furi_string_cat_printf(string, "%.3fG", (double)count / (1024 * 1024 * 1024));
    }
}

static void mass_storage_draw_callback(Canvas* canvas, void* _model) {
    MassStorageModel* model = _model;

    if(model->audio_mode) {
        mass_storage_draw_audio(canvas, model);
        return;
    }

    canvas_draw_icon(canvas, 8, 14, &I_Drive_112x35);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, canvas_width(canvas) / 2, 0, AlignCenter, AlignTop, "USB Mass Storage");

    canvas_set_font(canvas, FontSecondary);
    elements_string_fit_width(canvas, model->file_name, 89 - 2);
    canvas_draw_str_aligned(
        canvas, 50, 23, AlignCenter, AlignBottom, furi_string_get_cstr(model->file_name));

    furi_string_set_str(model->status_string, "R:");
    append_suffixed_byte_count(model->status_string, model->bytes_read);
    if(model->read_speed) {
        furi_string_cat_str(model->status_string, "; ");
        append_suffixed_byte_count(model->status_string, model->read_speed);
        furi_string_cat_str(model->status_string, "ps");
    }
    canvas_draw_str(canvas, 12, 34, furi_string_get_cstr(model->status_string));

    furi_string_set_str(model->status_string, "W:");
    append_suffixed_byte_count(model->status_string, model->bytes_written);
    if(model->write_speed) {
        furi_string_cat_str(model->status_string, "; ");
        append_suffixed_byte_count(model->status_string, model->write_speed);
        furi_string_cat_str(model->status_string, "ps");
    }
    canvas_draw_str(canvas, 12, 44, furi_string_get_cstr(model->status_string));

    if(model->wipe_active) {
        uint32_t percent = ((uint32_t)model->wipe_progress * 100 + UINT16_MAX / 2) / UINT16_MAX;
        furi_string_printf(model->status_string, "Wiping %lu%%", percent);
        elements_progress_bar_with_text(
            canvas,
            12,
            51,
            104,
            (float)model->wipe_progress / UINT16_MAX,
            furi_string_get_cstr(model->status_string));
    }
}

static bool mass_storage_input_callback(InputEvent* event, void* context) {
    MassStorage* mass_storage = context;
    if(!mass_storage->callback) return false;

    MassStorageInput input;
    bool consumed = true;
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        input = MassStorageInputPlayPause;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        input = MassStorageInputStop;
    } else if(event->key == InputKeyLeft && event->type == InputTypeShort) {
        input = MassStorageInputPrevious;
    } else if(event->key == InputKeyRight && event->type == InputTypeShort) {
        input = MassStorageInputNext;
    } else if(
        event->key == InputKeyUp &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        input = MassStorageInputVolumeUp;
    } else if(
        event->key == InputKeyDown &&
        (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
        input = MassStorageInputVolumeDown;
    } else if(event->key == InputKeyLeft && event->type == InputTypeLong) {
        input = MassStorageInputScanBackward;
    } else if(event->key == InputKeyRight && event->type == InputTypeLong) {
        input = MassStorageInputScanForward;
    } else if(
        (event->key == InputKeyLeft || event->key == InputKeyRight) &&
        event->type == InputTypeRelease) {
        input = MassStorageInputScanEnd;
    } else if(
        (event->key == InputKeyLeft || event->key == InputKeyRight || event->key == InputKeyUp ||
         event->key == InputKeyDown || event->key == InputKeyOk) &&
        (event->type == InputTypePress || event->type == InputTypeRepeat)) {
        return true;
    } else {
        consumed = false;
    }

    if(consumed) mass_storage->callback(input, mass_storage->context);
    return consumed;
}

MassStorage* mass_storage_alloc() {
    MassStorage* mass_storage = malloc(sizeof(MassStorage));
    memset(mass_storage, 0, sizeof(MassStorage));

    mass_storage->view = view_alloc();
    view_allocate_model(mass_storage->view, ViewModelTypeLocking, sizeof(MassStorageModel));
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        {
            model->file_name = furi_string_alloc();
            model->status_string = furi_string_alloc();
        },
        false);
    view_set_context(mass_storage->view, mass_storage);
    view_set_draw_callback(mass_storage->view, mass_storage_draw_callback);
    view_set_input_callback(mass_storage->view, mass_storage_input_callback);

    return mass_storage;
}

void mass_storage_free(MassStorage* mass_storage) {
    furi_assert(mass_storage);
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        {
            furi_string_free(model->file_name);
            furi_string_free(model->status_string);
        },
        false);
    view_free(mass_storage->view);
    free(mass_storage);
}

View* mass_storage_get_view(MassStorage* mass_storage) {
    furi_assert(mass_storage);
    return mass_storage->view;
}

void mass_storage_set_file_name(MassStorage* mass_storage, FuriString* name) {
    furi_assert(name);
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        { furi_string_set(model->file_name, name); },
        true);
}

void mass_storage_set_stats(MassStorage* mass_storage, uint32_t read, uint32_t written) {
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        {
            uint32_t now = furi_get_tick();
            model->read_speed = (read - model->bytes_read) * 1000 / (now - model->update_time);
            model->write_speed =
                (written - model->bytes_written) * 1000 / (now - model->update_time);
            model->bytes_read = read;
            model->bytes_written = written;
            model->update_time = now;
        },
        true);
}

void mass_storage_set_wipe_progress(MassStorage* mass_storage, uint16_t progress, bool active) {
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        {
            model->wipe_progress = progress;
            model->wipe_active = active;
        },
        true);
}

void mass_storage_set_audio_mode(MassStorage* mass_storage, bool enabled) {
    with_view_model(
        mass_storage->view, MassStorageModel * model, { model->audio_mode = enabled; }, true);
}

void mass_storage_set_audio_status(
    MassStorage* mass_storage,
    const SCSIAudioStatus* status,
    uint8_t track_count,
    uint32_t track_start,
    uint32_t track_end,
    uint8_t volume) {
    furi_assert(status);
    with_view_model(
        mass_storage->view,
        MassStorageModel * model,
        {
            model->audio_status = *status;
            model->track_count = track_count;
            model->track_start = track_start;
            model->track_end = track_end;
            model->volume = volume;
        },
        true);
}

void mass_storage_set_input_callback(
    MassStorage* mass_storage,
    MassStorageInputCallback callback,
    void* context) {
    mass_storage->callback = callback;
    mass_storage->context = context;
}
