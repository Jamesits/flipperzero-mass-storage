#pragma once

#include <furi.h>

#define SCSI_BLOCK_SIZE (0x200UL)

#define SCSI_SK_NOT_READY       (2)
#define SCSI_SK_MEDIUM_ERROR    (3)
#define SCSI_SK_ILLEGAL_REQUEST (5)
#define SCSI_SK_DATA_PROTECT    (7)

#define SCSI_ASC_LOGICAL_UNIT_NOT_READY          (0x04)
#define SCSI_ASC_WRITE_ERROR                     (0x0C)
#define SCSI_ASC_UNRECOVERED_READ_ERROR          (0x11)
#define SCSI_ASC_INVALID_COMMAND_OPERATION_CODE  (0x20)
#define SCSI_ASC_LBA_OOB                         (0x21)
#define SCSI_ASC_INVALID_FIELD_IN_CDB            (0x24)
#define SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST (0x26)
#define SCSI_ASC_WRITE_PROTECTED                 (0x27)
#define SCSI_ASC_ILLEGAL_MODE_FOR_TRACK          (0x64)

#define SCSI_ASCQ_OPERATION_IN_PROGRESS (0x07)

typedef enum {
    MassStorageDeviceTypeUsbSsd,
    MassStorageDeviceTypeUsbHdd,
    MassStorageDeviceTypeFdd,
    MassStorageDeviceTypeOptical,
    MassStorageDeviceTypeCount,
} MassStorageDeviceType;

typedef enum {
    SCSIAudioStatusNone = 0x00,
    SCSIAudioStatusPlaying = 0x11,
    SCSIAudioStatusPaused = 0x12,
    SCSIAudioStatusCompleted = 0x13,
    SCSIAudioStatusError = 0x14,
    SCSIAudioStatusStopped = 0x15,
} SCSIAudioStatusCode;

typedef enum {
    SCSIAudioScanNone,
    SCSIAudioScanForward,
    SCSIAudioScanBackward,
} SCSIAudioScanDirection;

typedef enum {
    SCSIAudioControlPlay,
    SCSIAudioControlPause,
    SCSIAudioControlResume,
    SCSIAudioControlStop,
    SCSIAudioControlScanForward,
    SCSIAudioControlScanBackward,
    SCSIAudioControlScanEnd,
    SCSIAudioControlSeek,
} SCSIAudioControl;

typedef struct {
    uint8_t number;
    uint32_t start_lba;
    uint32_t end_lba;
    uint32_t index0_lba;
    bool has_index0;
} SCSIAudioTrackInfo;

typedef struct {
    SCSIAudioStatusCode status;
    SCSIAudioScanDirection scan_direction;
    uint32_t lba;
    uint32_t end_lba;
    uint8_t track;
    uint8_t index;
} SCSIAudioStatus;

typedef struct {
    void* ctx;
    bool (*read)(
        void* ctx,
        uint32_t lba,
        uint32_t count,
        uint8_t* out,
        uint32_t* out_len,
        uint32_t out_cap);
    bool (*write)(void* ctx, uint32_t lba, uint16_t count, uint8_t* buf, uint32_t len);
    uint32_t (*num_blocks)(void* ctx);
    bool (*sync)(void* ctx);
    void (*eject)(void* ctx);
    // Reports a full-medium optical wipe. Progress ranges from 0 to UINT16_MAX.
    // active is false on completion, failure, or cancellation. May be NULL.
    void (*wipe_progress)(void* ctx, uint16_t progress, bool active);
    // Called when the host tears down the USB device (SetConfiguration 0), as opposed
    // to ejecting just the media via eject(). May be NULL.
    void (*removed)(void* ctx);
    // Called on USB bus suspend, the closest signal to a physical port/cable disconnect
    // (the bus goes idle when unplugged). Also fires on host sleep. May be NULL.
    void (*suspended)(void* ctx);
    // Called when the USB device becomes configured, suspended, resumed, or deconfigured.
    // May be NULL.
    void (*connection_changed)(void* ctx, bool connected);
    uint8_t (*audio_track_count)(void* ctx);
    bool (*audio_track_info)(void* ctx, uint8_t track, SCSIAudioTrackInfo* info);
    bool (*audio_get_status)(void* ctx, SCSIAudioStatus* status);
    bool (
        *audio_control)(void* ctx, SCSIAudioControl control, uint32_t start_lba, uint32_t end_lba);
    bool read_only;
    // Advertise removable media so the host offers eject (required for exit on eject).
    bool removable;
    MassStorageDeviceType device_type;
    uint32_t block_size;
    bool audio_cd;
    // The backing image already contains a formatted rewritable optical filesystem.
    bool optical_formatted;
} SCSIDeviceFunc;

typedef struct {
    SCSIDeviceFunc fn;

    uint8_t* cmd;
    uint8_t cmd_len;
    bool rx_done;
    bool tx_done;
    bool phase_error;
    bool eject_pending;

    uint8_t sk; // sense key
    uint8_t asc; // additional sense code
    uint8_t ascq; // additional sense code qualifier

    // command-specific data
    // valid from cmd_start to cmd_end
    union {
        struct {
            uint32_t count;
            uint32_t lba;
            uint32_t block_size;
        } read;

        struct {
            uint32_t count;
            uint32_t lba;
        } write;

        struct {
            uint16_t remaining;
        } mode_select;

        struct {
            uint32_t total;
            uint32_t remaining;
            uint8_t parameters[12];
        } format;

        struct {
            uint32_t remaining;
        } cue;
    };

    uint32_t next_writable_lba;
    uint32_t reserved_blocks;
    uint32_t formatted_blocks;
    uint32_t optical_packet_size;
    bool optical_open;
    bool optical_finalized;
    bool optical_formatted;

    struct {
        uint32_t lba;
        uint32_t total_blocks;
        uint16_t progress;
        bool active;
        bool failed;
        bool immediate;
        bool operational_change_pending;
        bool busy_change_pending;
    } blank;
} SCSISession;

bool scsi_is_usb_disk(MassStorageDeviceType device_type);
void scsi_session_init(SCSISession* scsi, SCSIDeviceFunc fn);

bool scsi_cmd_start(
    SCSISession* scsi,
    uint8_t* cmd,
    uint8_t len,
    uint32_t transfer_len,
    bool device_to_host);
bool scsi_cmd_rx_data(SCSISession* scsi, uint8_t* data, uint32_t len);
bool scsi_cmd_tx_data(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap);
bool scsi_cmd_end(SCSISession* scsi);

bool scsi_blank_in_progress(const SCSISession* scsi);
bool scsi_blank_defers_status(const SCSISession* scsi);
bool scsi_blank_succeeded(const SCSISession* scsi);
void scsi_blank_step(SCSISession* scsi, uint8_t* buffer, uint32_t buffer_size);
void scsi_blank_cancel(SCSISession* scsi);
