#include "../mass_storage_app_i.h"

static const char* device_type_names[MassStorageDeviceTypeCount] = {
    [MassStorageDeviceTypeUsbSsd] = "USB-SSD",
    [MassStorageDeviceTypeUsbHdd] = "USB-HDD",
    [MassStorageDeviceTypeFdd] = "FDD",
    [MassStorageDeviceTypeOptical] = "Optical",
};

static const char* exit_on_eject_names[MassStorageExitOnEjectCount] = {
    [MassStorageExitOnEjectOff] = "Off",
    [MassStorageExitOnEjectDisk] = "Disk",
    [MassStorageExitOnEjectUsb] = "USB",
};

static const char* audio_output_names[AudioCdOutputCount] = {
    [AudioCdOutputOff] = "Off",
    [AudioCdOutputSpeaker] = "Speaker",
    [AudioCdOutputExternal] = "External",
    [AudioCdOutputBoth] = "Both",
};

static void mass_storage_settings_select(void* context, uint32_t index) {
    MassStorageApp* app = context;
    if(index == 0) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MassStorageCustomEventStart);
    }
}

static void mass_storage_read_only(VariableItem* item) {
    MassStorageApp* app = variable_item_get_context(item);
    app->read_only = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, app->read_only ? "On" : "Off");
}

static void mass_storage_exit_on_eject(VariableItem* item) {
    MassStorageApp* app = variable_item_get_context(item);
    app->exit_on_eject = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, exit_on_eject_names[app->exit_on_eject]);
}

static void mass_storage_device_type(VariableItem* item) {
    MassStorageApp* app = variable_item_get_context(item);
    app->device_type = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, device_type_names[app->device_type]);
}

static void mass_storage_audio_output(VariableItem* item) {
    MassStorageApp* app = variable_item_get_context(item);
    app->audio_output = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, audio_output_names[app->audio_output]);
}

void mass_storage_scene_settings_on_enter(void* context) {
    MassStorageApp* app = context;
    bool is_iso = furi_string_end_withi(app->file_path, MASS_STORAGE_ISO_EXTENSION);
    bool is_cue = furi_string_end_withi(app->file_path, MASS_STORAGE_CUE_EXTENSION);
    bool fixed_optical = is_iso || is_cue;
    if(fixed_optical) {
        app->read_only = true;
        app->device_type = MassStorageDeviceTypeOptical;
    }

    variable_item_list_add(app->variable_item_list, "Start", 0, NULL, NULL);

    VariableItem* read_only_item = variable_item_list_add(
        app->variable_item_list,
        "Read only",
        fixed_optical ? 1 : 2,
        fixed_optical ? NULL : mass_storage_read_only,
        app);

    VariableItem* exit_on_eject_item = variable_item_list_add(
        app->variable_item_list,
        "Exit on eject",
        MassStorageExitOnEjectCount,
        mass_storage_exit_on_eject,
        app);

    VariableItem* device_type_item = variable_item_list_add(
        app->variable_item_list,
        "Report as",
        fixed_optical ? 1 : MassStorageDeviceTypeCount,
        fixed_optical ? NULL : mass_storage_device_type,
        app);

    if(is_cue) {
        VariableItem* audio_output_item = variable_item_list_add(
            app->variable_item_list,
            "Local output",
            AudioCdOutputCount,
            mass_storage_audio_output,
            app);
        variable_item_set_current_value_index(audio_output_item, app->audio_output);
        variable_item_set_current_value_text(
            audio_output_item, audio_output_names[app->audio_output]);
    }

    variable_item_list_set_enter_callback(
        app->variable_item_list, mass_storage_settings_select, app);

    variable_item_set_current_value_index(read_only_item, fixed_optical ? 0 : app->read_only);
    variable_item_set_current_value_text(read_only_item, app->read_only ? "On" : "Off");
    variable_item_set_current_value_index(exit_on_eject_item, app->exit_on_eject);
    variable_item_set_current_value_text(
        exit_on_eject_item, exit_on_eject_names[app->exit_on_eject]);
    variable_item_set_current_value_index(device_type_item, fixed_optical ? 0 : app->device_type);
    variable_item_set_current_value_text(
        device_type_item, is_cue ? "Audio CD" : device_type_names[app->device_type]);

    mass_storage_app_show_loading_popup(app, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, MassStorageAppViewStart);
}

bool mass_storage_scene_settings_on_event(void* context, SceneManagerEvent event) {
    MassStorageApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == MassStorageCustomEventStart) {
        if(!furi_hal_usb_is_locked()) {
            scene_manager_next_scene(app->scene_manager, MassStorageSceneWork);
        } else {
            scene_manager_next_scene(app->scene_manager, MassStorageSceneUsbLocked);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, MassStorageSceneStart);
    }

    return consumed;
}

void mass_storage_scene_settings_on_exit(void* context) {
    MassStorageApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
