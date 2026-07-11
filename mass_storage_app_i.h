#pragma once

#include "mass_storage_app.h"
#include "scenes/mass_storage_scene.h"
#include "helpers/mass_storage_usb.h"

#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <dialogs/dialogs.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/loading.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include "views/mass_storage_view.h"
#include <mass_storage_icons.h>

#define MASS_STORAGE_APP_PATH_FOLDER      STORAGE_APP_DATA_PATH_PREFIX
#define MASS_STORAGE_APP_EXTENSION        ".img"
#define MASS_STORAGE_APP_IMAGE_EXTENSIONS ".img|.iso"
#define MASS_STORAGE_ISO_EXTENSION        ".iso"
#define MASS_STORAGE_FILE_NAME_LEN        40
#define MASS_STORAGE_MAX_FILE_PARTS       4
#define MASS_STORAGE_FILE_PART_SIZE       (2ull * 1024 * 1024 * 1024)

typedef enum {
    MassStorageExitOnEjectOff, // never exit
    MassStorageExitOnEjectDisk, // exit when the host ejects the disk (SCSI media eject)
    MassStorageExitOnEjectUsb, // additionally exit when the host removes the USB device
    MassStorageExitOnEjectCount,
} MassStorageExitOnEject;

struct MassStorageApp {
    Gui* gui;
    Storage* fs_api;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Widget* widget;
    DialogsApp* dialogs;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    Loading* loading;
    NotificationApp* notifications;

    FuriString* file_path;
    File* files[MASS_STORAGE_MAX_FILE_PARTS];
    uint64_t file_sizes[MASS_STORAGE_MAX_FILE_PARTS];
    uint64_t file_offsets[MASS_STORAGE_MAX_FILE_PARTS];
    uint8_t file_count;
    MassStorage* mass_storage_view;

    FuriMutex* usb_mutex;
    MassStorageUsb* usb;

    char new_file_name[MASS_STORAGE_FILE_NAME_LEN + 1];
    uint64_t new_file_size;
    bool read_only;
    MassStorageExitOnEject exit_on_eject;
    MassStorageDeviceType device_type;

    uint32_t bytes_read, bytes_written;
    uint32_t led_bytes_read, led_bytes_written;
    uint16_t wipe_progress;
    bool led_blinking;
    bool wipe_active;
};

typedef enum {
    MassStorageAppViewStart,
    MassStorageAppViewTextInput,
    MassStorageAppViewWork,
    MassStorageAppViewLoading,
    MassStorageAppViewWidget,
} MassStorageAppView;

enum MassStorageCustomEvent {
    // Reserve first 100 events for button types and indexes, starting from 0
    MassStorageCustomEventReserved = 100,

    MassStorageCustomEventEject,
    MassStorageCustomEventFileSelect,
    MassStorageCustomEventNewImage,
    MassStorageCustomEventNameInput,
    MassStorageCustomEventStart,
};

void mass_storage_app_show_loading_popup(MassStorageApp* app, bool show);
bool mass_storage_file_seek(File* file, uint64_t offset);
