#pragma once

#include <furi.h>

#define SCSI_BLOCK_SIZE (0x200UL)

#define SCSI_SK_ILLEGAL_REQUEST (5)
#define SCSI_SK_DATA_PROTECT    (7)

#define SCSI_ASC_INVALID_COMMAND_OPERATION_CODE (0x20)
#define SCSI_ASC_LBA_OOB                        (0x21)
#define SCSI_ASC_INVALID_FIELD_IN_CDB           (0x24)
#define SCSI_ASC_WRITE_PROTECTED                (0x27)

typedef enum {
    MassStorageDeviceTypeUsbSsd,
    MassStorageDeviceTypeUsbHdd,
    MassStorageDeviceTypeFdd,
    MassStorageDeviceTypeOptical,
    MassStorageDeviceTypeCount,
} MassStorageDeviceType;

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
    // Called when the host tears down the USB device (SetConfiguration 0), as opposed
    // to ejecting just the media via eject(). May be NULL.
    void (*removed)(void* ctx);
    bool read_only;
    // Advertise removable media so the host offers eject (required for exit on eject).
    bool removable;
    MassStorageDeviceType device_type;
    uint32_t block_size;
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

    // command-specific data
    // valid from cmd_start to cmd_end
    union {
        struct {
            uint32_t count;
            uint32_t lba;
        } read;

        struct {
            uint32_t count;
            uint32_t lba;
        } write;

        struct {
            uint16_t remaining;
        } mode_select;

        struct {
            uint16_t remaining;
        } format;
    };

    uint32_t next_writable_lba;
    uint32_t reserved_blocks;
    bool optical_open;
    bool optical_finalized;
    bool optical_formatted;
} SCSISession;

bool scsi_is_usb_disk(MassStorageDeviceType device_type);

bool scsi_cmd_start(
    SCSISession* scsi,
    uint8_t* cmd,
    uint8_t len,
    uint32_t transfer_len,
    bool device_to_host);
bool scsi_cmd_rx_data(SCSISession* scsi, uint8_t* data, uint32_t len);
bool scsi_cmd_tx_data(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap);
bool scsi_cmd_end(SCSISession* scsi);
