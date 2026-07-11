#include "../mass_storage_app_i.h"

static const char* device_type_names[MassStorageDeviceTypeCount] = {
    [MassStorageDeviceTypeUsbSsd] = "USB-SSD",
    [MassStorageDeviceTypeUsbHdd] = "USB-HDD",
    [MassStorageDeviceTypeFdd] = "FDD",
    [MassStorageDeviceTypeOptical] = "Optical",
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
    variable_item_set_current_value_text(item, app->exit_on_eject ? "On" : "Off");
}

static void mass_storage_device_type(VariableItem* item) {
    MassStorageApp* app = variable_item_get_context(item);
    app->device_type = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, device_type_names[app->device_type]);
}

void mass_storage_scene_settings_on_enter(void* context) {
    MassStorageApp* app = context;
    bool is_iso = furi_string_end_withi(app->file_path, MASS_STORAGE_ISO_EXTENSION);
    if(is_iso) {
        app->read_only = true;
        app->device_type = MassStorageDeviceTypeOptical;
    }

    variable_item_list_add(app->variable_item_list, "Start", 0, NULL, NULL);

    VariableItem* read_only_item = variable_item_list_add(
        app->variable_item_list,
        "Read only",
        is_iso ? 1 : 2,
        is_iso ? NULL : mass_storage_read_only,
        app);

    VariableItem* exit_on_eject_item = variable_item_list_add(
        app->variable_item_list, "Exit on eject", 2, mass_storage_exit_on_eject, app);

    VariableItem* device_type_item = variable_item_list_add(
        app->variable_item_list,
        "Report as",
        is_iso ? 1 : MassStorageDeviceTypeCount,
        is_iso ? NULL : mass_storage_device_type,
        app);

    variable_item_list_set_enter_callback(
        app->variable_item_list, mass_storage_settings_select, app);

    variable_item_set_current_value_index(read_only_item, is_iso ? 0 : app->read_only);
    variable_item_set_current_value_text(read_only_item, app->read_only ? "On" : "Off");
    variable_item_set_current_value_index(exit_on_eject_item, app->exit_on_eject);
    variable_item_set_current_value_text(exit_on_eject_item, app->exit_on_eject ? "On" : "Off");
    variable_item_set_current_value_index(device_type_item, is_iso ? 0 : app->device_type);
    variable_item_set_current_value_text(device_type_item, device_type_names[app->device_type]);

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
