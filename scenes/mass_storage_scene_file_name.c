#include "../mass_storage_app_i.h"

#define WRITE_BUF_LEN 4096

static void mass_storage_file_name_text_callback(void* context) {
    furi_assert(context);

    MassStorageApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventNameInput);
}

static bool
    mass_storage_create_image_part(Storage* storage, const char* file_path, uint64_t size) {
    File* file = storage_file_alloc(storage);

    bool success = false;
    uint8_t* buffer = malloc(WRITE_BUF_LEN);
    memset(buffer, 0, WRITE_BUF_LEN);
    do {
        if(!storage_file_open(file, file_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;
        if(!mass_storage_file_seek(file, size)) break;
        if(!mass_storage_file_seek(file, 0)) break;
        // Zero out first 4k - partition table and adjacent data
        if(!storage_file_write(file, buffer, WRITE_BUF_LEN)) break;

        success = true;
    } while(false);

    free(buffer);
    storage_file_close(file);
    storage_file_free(file);
    return success;
}

static bool mass_storage_create_image(Storage* storage, const char* file_path, uint64_t size) {
    FURI_LOG_I("TAG", "Creating image %s, len:%llu", file_path, size);
    mass_storage_metadata_remove(storage, file_path);
    FuriString* part_path = furi_string_alloc();
    uint8_t part_count = (size + MASS_STORAGE_FILE_PART_SIZE - 1) / MASS_STORAGE_FILE_PART_SIZE;
    bool success = true;

    for(uint8_t part = 0; part < part_count; part++) {
        if(part == 0) {
            furi_string_set_str(part_path, file_path);
        } else {
            furi_string_printf(part_path, "%s.%u", file_path, part);
        }

        uint64_t part_size = MIN(size, MASS_STORAGE_FILE_PART_SIZE);
        if(!mass_storage_create_image_part(storage, furi_string_get_cstr(part_path), part_size)) {
            success = false;
            break;
        }
        size -= part_size;
    }

    for(uint8_t part = part_count; success && part < MASS_STORAGE_MAX_FILE_PARTS; part++) {
        if(part == 0) {
            furi_string_set_str(part_path, file_path);
        } else {
            furi_string_printf(part_path, "%s.%u", file_path, part);
        }
        if(storage_file_exists(storage, furi_string_get_cstr(part_path)) &&
           storage_common_remove(storage, furi_string_get_cstr(part_path)) != FSE_OK) {
            success = false;
        }
    }

    if(!success) {
        for(uint8_t part = 0; part < MASS_STORAGE_MAX_FILE_PARTS; part++) {
            if(part == 0) {
                furi_string_set_str(part_path, file_path);
            } else {
                furi_string_printf(part_path, "%s.%u", file_path, part);
            }
            storage_common_remove(storage, furi_string_get_cstr(part_path));
        }
    }

    furi_string_free(part_path);
    return success;
}

void mass_storage_scene_file_name_on_enter(void* context) {
    MassStorageApp* app = context;

    text_input_set_header_text(app->text_input, "Enter image name");
    ValidatorIsFile* validator_is_file =
        validator_is_file_alloc_init(MASS_STORAGE_APP_PATH_FOLDER, MASS_STORAGE_APP_EXTENSION, "");
    text_input_set_validator(app->text_input, validator_is_file_callback, validator_is_file);

    text_input_set_result_callback(
        app->text_input,
        mass_storage_file_name_text_callback,
        app,
        app->new_file_name,
        MASS_STORAGE_FILE_NAME_LEN,
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, MassStorageAppViewTextInput);
}

bool mass_storage_scene_file_name_on_event(void* context, SceneManagerEvent event) {
    UNUSED(event);
    MassStorageApp* app = context;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MassStorageCustomEventNameInput) {
            mass_storage_app_show_loading_popup(app, true);
            furi_string_printf(
                app->file_path,
                "%s/%s%s",
                MASS_STORAGE_APP_PATH_FOLDER,
                app->new_file_name,
                MASS_STORAGE_APP_EXTENSION);
            if(mass_storage_create_image(
                   app->fs_api, furi_string_get_cstr(app->file_path), app->new_file_size)) {
                scene_manager_next_scene(app->scene_manager, MassStorageSceneSettings);
            } // TODO: error message screen
        }
    }
    return consumed;
}

void mass_storage_scene_file_name_on_exit(void* context) {
    UNUSED(context);
    MassStorageApp* app = context;
    void* validator_context = text_input_get_validator_callback_context(app->text_input);
    text_input_set_validator(app->text_input, NULL, NULL);
    validator_is_file_free(validator_context);
    text_input_reset(app->text_input);
}
