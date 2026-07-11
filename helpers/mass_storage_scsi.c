#include "mass_storage_scsi.h"

#include <core/log.h>

#define TAG "MassStorageSCSI"

#define SCSI_TEST_UNIT_READY        (0x00)
#define SCSI_REQUEST_SENSE          (0x03)
#define SCSI_INQUIRY                (0x12)
#define SCSI_READ_FORMAT_CAPACITIES (0x23)
#define SCSI_READ_CAPACITY_10       (0x25)
#define SCSI_MODE_SENSE_6           (0x1A)
#define SCSI_READ_10                (0x28)
#define SCSI_VERIFY_10              (0x2F)
#define SCSI_SYNCHRONIZE_CACHE_10   (0x35)
#define SCSI_READ_TOC               (0x43)
#define SCSI_READ_HEADER            (0x44)
#define SCSI_GET_CONFIGURATION      (0x46)
#define SCSI_GET_EVENT_STATUS       (0x4A)
#define SCSI_READ_DISC_INFORMATION  (0x51)
#define SCSI_READ_TRACK_INFORMATION (0x52)
#define SCSI_RESERVE_TRACK          (0x53)
#define SCSI_MODE_SELECT_10         (0x55)
#define SCSI_MODE_SENSE_10          (0x5A)
#define SCSI_CLOSE_TRACK_SESSION    (0x5B)
#define SCSI_READ_12                (0xA8)
#define SCSI_WRITE_12               (0xAA)
#define SCSI_SET_CD_SPEED           (0xBB)
#define SCSI_PREVENT_MEDIUM_REMOVAL (0x1E)
#define SCSI_START_STOP_UNIT        (0x1B)
#define SCSI_WRITE_10               (0x2A)

static const uint8_t scsi_peripheral_device_type[MassStorageDeviceTypeCount] = {
    [MassStorageDeviceTypeUsbSsd] = 0x00,
    [MassStorageDeviceTypeUsbHdd] = 0x00,
    [MassStorageDeviceTypeFdd] = 0x00,
    [MassStorageDeviceTypeOptical] = 0x05,
};

static const char scsi_product_id[MassStorageDeviceTypeCount][17] = {
    [MassStorageDeviceTypeUsbSsd] = "USB SSD         ",
    [MassStorageDeviceTypeUsbHdd] = "USB HDD         ",
    [MassStorageDeviceTypeFdd] = "Floppy Drive    ",
    [MassStorageDeviceTypeOptical] = "Optical Drive   ",
};

static bool scsi_is_usb_disk(MassStorageDeviceType device_type) {
    return device_type == MassStorageDeviceTypeUsbSsd ||
           device_type == MassStorageDeviceTypeUsbHdd;
}

static bool scsi_tx_response(
    SCSISession* scsi,
    uint8_t* data,
    uint32_t* len,
    uint32_t cap,
    const uint8_t* response,
    uint32_t response_len) {
    *len = MIN(cap, response_len);
    memcpy(data, response, *len);
    scsi->tx_done = true;
    return true;
}

static void scsi_store_cdrom_address(uint8_t* data, uint32_t lba, bool msf) {
    if(msf) {
        lba += 150;
        data[0] = 0;
        data[1] = lba / (60 * 75);
        data[2] = (lba / 75) % 60;
        data[3] = lba % 75;
    } else {
        data[0] = lba >> 24;
        data[1] = lba >> 16;
        data[2] = lba >> 8;
        data[3] = lba;
    }
}

static uint32_t scsi_read_be32(const uint8_t* data) {
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static void scsi_store_be32(uint8_t* data, uint32_t value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static uint8_t
    scsi_mode_page(uint8_t* page, uint8_t page_code, uint8_t page_control, bool read_only) {
    if(page_code == 0x01) {
        page[0] = 0x01;
        page[1] = 10;
        return 12;
    } else if(page_code == 0x05) {
        page[0] = 0x05;
        page[1] = 50;
        if(page_control == 1) {
            page[2] = 0x2F;
            page[3] = 0x0F;
            page[4] = 0x0F;
            page[5] = 0xFF;
        } else {
            page[2] = 0x21; // valid link size, track-at-once
            page[3] = 0x04; // Mode 1 data track
            page[4] = 0x08; // 2048-byte Mode 1 user data
            page[5] = 0x07;
            page[14] = 0x00;
            page[15] = 0x96;
        }
        return 52;
    } else if(page_code == 0x08) {
        page[0] = 0x08;
        page[1] = 10;
        // Do not advertise a volatile write cache. Reads remain cached.
        return 12;
    } else if(page_code == 0x2A) {
        page[0] = 0x2A;
        page[1] = 30;
        if(page_control != 1) {
            page[2] = 0x01; // read CD-R
            page[3] = read_only ? 0x00 : 0x01; // write CD-R
            page[6] = 0x29; // tray, eject and lock supported
            page[8] = 0x1B; // 7056 KiB/s read speed
            page[9] = 0x90;
            page[12] = 0x00;
            page[13] = 0x10; // 16 KiB buffer
            page[14] = 0x1B;
            page[15] = 0x90;
            page[18] = 0x1B; // 7056 KiB/s write speed
            page[19] = 0x90;
            page[20] = 0x1B;
            page[21] = 0x90;
            page[28] = 0x1B;
            page[29] = 0x90;
        }
        return 32;
    }
    return 0;
}

static bool
    scsi_mode_sense(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap, bool ten_byte) {
    uint8_t response[120] = {0};
    uint8_t header_len = ten_byte ? 8 : 4;
    uint8_t response_len = header_len;

    if(ten_byte) {
        response[3] = scsi->fn.read_only ? 0x80 : 0;
    } else {
        response[2] = scsi->fn.read_only ? 0x80 : 0;
    }

    if(scsi->fn.device_type == MassStorageDeviceTypeOptical) {
        uint8_t page_control = scsi->cmd[2] >> 6;
        uint8_t page_code = scsi->cmd[2] & 0x3F;
        if(page_control == 3) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }

        const uint8_t pages[] = {0x01, 0x05, 0x08, 0x2A};
        for(uint8_t i = 0; i < COUNT_OF(pages); i++) {
            if(page_code == pages[i] || page_code == 0x3F) {
                response_len += scsi_mode_page(
                    response + response_len, pages[i], page_control, scsi->fn.read_only);
            }
        }
        if(response_len == header_len) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
    }

    if(ten_byte) {
        response[0] = (response_len - 2) >> 8;
        response[1] = response_len - 2;
    } else {
        response[0] = response_len - 1;
    }
    return scsi_tx_response(scsi, data, len, cap, response, response_len);
}

static bool scsi_start_write(
    SCSISession* scsi,
    uint32_t lba,
    uint32_t count,
    uint32_t transfer_len,
    bool device_to_host) {
    if(scsi->fn.read_only) {
        scsi->sk = SCSI_SK_DATA_PROTECT;
        scsi->asc = SCSI_ASC_WRITE_PROTECTED;
        return false;
    }
    uint64_t expected_len = (uint64_t)count * scsi->fn.block_size;
    if(device_to_host || expected_len > UINT32_MAX || transfer_len != expected_len) {
        scsi->phase_error = true;
        return false;
    }
    uint32_t num_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
    if(lba > num_blocks || count > num_blocks - lba ||
       (scsi->fn.device_type == MassStorageDeviceTypeOptical &&
        (scsi->optical_finalized || lba != scsi->next_writable_lba))) {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_LBA_OOB;
        return false;
    }
    scsi->write.lba = lba;
    scsi->write.count = count;
    scsi->rx_done = count == 0;
    return true;
}

bool scsi_cmd_start(
    SCSISession* scsi,
    uint8_t* cmd,
    uint8_t len,
    uint32_t transfer_len,
    bool device_to_host) {
    scsi->phase_error = false;
    if(!len) {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }
    FURI_LOG_T(TAG, "START %02X", cmd[0]);
    scsi->cmd = cmd;
    scsi->cmd_len = len;
    scsi->rx_done = false;
    scsi->tx_done = false;
    switch(cmd[0]) {
    case SCSI_WRITE_10: {
        if(len < 10) return false;
        uint32_t lba = scsi_read_be32(cmd + 2);
        uint32_t count = cmd[7] << 8 | cmd[8];
        FURI_LOG_D(TAG, "SCSI_WRITE_10 %08lX %04lX", lba, count);
        return scsi_start_write(scsi, lba, count, transfer_len, device_to_host);
    }; break;
    case SCSI_WRITE_12: {
        if(len < 12) return false;
        uint32_t lba = scsi_read_be32(cmd + 2);
        uint32_t count = scsi_read_be32(cmd + 6);
        FURI_LOG_D(TAG, "SCSI_WRITE_12 %08lX %08lX", lba, count);
        return scsi_start_write(scsi, lba, count, transfer_len, device_to_host);
    }; break;
    case SCSI_MODE_SELECT_10: {
        if(len < 10 || !(cmd[1] & 0x10) || (cmd[1] & 0x01)) return false;
        scsi->mode_select.remaining = cmd[7] << 8 | cmd[8];
        if(device_to_host || transfer_len != scsi->mode_select.remaining) {
            scsi->phase_error = true;
            return false;
        }
        scsi->rx_done = scsi->mode_select.remaining == 0;
        return true;
    }; break;
    case SCSI_READ_10: {
        if(len < 10) return false;
        scsi->read.lba = scsi_read_be32(cmd + 2);
        scsi->read.count = cmd[7] << 8 | cmd[8];
        scsi->tx_done = scsi->read.count == 0;
        FURI_LOG_D(TAG, "SCSI_READ_10 %08lX %04lX", scsi->read.lba, scsi->read.count);
        return true;
    }; break;
    case SCSI_READ_12: {
        if(len < 12) return false;
        scsi->read.lba = scsi_read_be32(cmd + 2);
        scsi->read.count = scsi_read_be32(cmd + 6);
        scsi->tx_done = scsi->read.count == 0;
        FURI_LOG_D(TAG, "SCSI_READ_12 %08lX %08lX", scsi->read.lba, scsi->read.count);
        return true;
    }; break;
    }
    return true;
}

bool scsi_cmd_rx_data(SCSISession* scsi, uint8_t* data, uint32_t len) {
    FURI_LOG_T(TAG, "RX %02X len %lu", scsi->cmd[0], len);
    if(scsi->rx_done) return false;
    switch(scsi->cmd[0]) {
    case SCSI_WRITE_10:
    case SCSI_WRITE_12: {
        uint32_t block_size = scsi->fn.block_size;
        if(len % block_size) return false;
        uint16_t blocks = len / block_size;
        if(blocks > scsi->write.count) return false;
        bool result =
            scsi->fn.write(scsi->fn.ctx, scsi->write.lba, blocks, data, blocks * block_size);
        if(!result) return false;
        scsi->write.lba += blocks;
        scsi->write.count -= blocks;
        if(scsi->fn.device_type == MassStorageDeviceTypeOptical) {
            scsi->next_writable_lba += blocks;
            scsi->optical_open = true;
        }
        if(!scsi->write.count) {
            scsi->rx_done = true;
        }
        return true;
    }; break;
    case SCSI_MODE_SELECT_10: {
        if(len > scsi->mode_select.remaining) return false;
        // Windows sends an 8-byte header followed by Write Parameters page 05h.
        if(scsi->mode_select.remaining == (uint16_t)(scsi->cmd[7] << 8 | scsi->cmd[8])) {
            if(len < 10 || data[6] || data[7] || (data[8] & 0x3F) != 0x05 || data[9] < 3 ||
               (data[10] & 0x0F) > 1 || (data[11] & 0x20) || (data[12] & 0x0F) != 0x08) {
                scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
                scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
                return false;
            }
        }
        scsi->mode_select.remaining -= len;
        scsi->rx_done = scsi->mode_select.remaining == 0;
        return true;
    }; break;
    default: {
        FURI_LOG_W(TAG, "unexpected scsi rx data cmd=%02X", scsi->cmd[0]);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }; break;
    }
}

bool scsi_cmd_tx_data(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap) {
    FURI_LOG_T(TAG, "TX %02X cap %lu", scsi->cmd[0], cap);
    if(scsi->tx_done) return false;
    switch(scsi->cmd[0]) {
    case SCSI_REQUEST_SENSE: {
        FURI_LOG_D(TAG, "SCSI_REQUEST_SENSE");
        if(cap < 18) return false;
        memset(data, 0, cap);
        data[0] = 0x70; // fixed format sense data
        data[1] = 0; // obsolete
        data[2] = scsi->sk; // sense key
        data[3] = 0; // information
        data[4] = 0; // information
        data[5] = 0; // information
        data[6] = 0; // information
        data[7] = 10; // additional sense length (len-8)
        data[8] = 0; // command specific information
        data[9] = 0; // command specific information
        data[10] = 0; // command specific information
        data[11] = 0; // command specific information
        data[12] = scsi->asc; // additional sense code
        data[13] = 0; // additional sense code qualifier
        data[14] = 0; // field replaceable unit code
        data[15] = 0; // sense key specific information
        data[16] = 0; // sense key specific information
        data[17] = 0; // sense key specific information
        *len = 18;
        scsi->sk = 0;
        scsi->asc = 0;
        scsi->tx_done = true;
        return true;
    }; break;
    case SCSI_INQUIRY: {
        FURI_LOG_D(TAG, "SCSI_INQUIRY");
        if(scsi->cmd_len < 5) return false;

        bool evpd = scsi->cmd[1] & 1;
        uint8_t page_code = scsi->cmd[2];
        if(evpd == 0) {
            if(page_code != 0) return false;

            uint8_t response[36] = {0};
            response[0] = scsi_peripheral_device_type[scsi->fn.device_type];
            response[1] = 0x80; // removable: true
            response[2] = scsi_is_usb_disk(scsi->fn.device_type) ? 0x06 : 0x04;
            response[3] = 0x02; // response data format
            response[4] = 31; // additional length (len - 5)
            memcpy(response + 8, "Flipper ", 8); // vendor id
            memcpy(response + 16, scsi_product_id[scsi->fn.device_type], 16); // product id
            memcpy(response + 32, "0001", 4); // product revision level
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }

        if(page_code == 0x00) {
            uint8_t response[7] = {
                scsi_peripheral_device_type[scsi->fn.device_type],
                0x00,
                0x00,
                0x02,
                0x00,
                0x80,
                0xB1};
            uint8_t response_len = scsi_is_usb_disk(scsi->fn.device_type) ? 7 : 6;
            response[3] = response_len - 4;
            return scsi_tx_response(scsi, data, len, cap, response, response_len);
        } else if(page_code == 0x80) {
            const uint8_t response[5] = {
                scsi_peripheral_device_type[scsi->fn.device_type], 0x80, 0x00, 0x01, '0'};
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        } else if(page_code == 0xB1 && scsi_is_usb_disk(scsi->fn.device_type)) {
            uint8_t response[64] = {0};
            uint16_t rotation_rate = scsi->fn.device_type == MassStorageDeviceTypeUsbSsd ? 1 :
                                                                                           7200;
            response[0] = scsi_peripheral_device_type[scsi->fn.device_type];
            response[1] = 0xB1;
            response[3] = 0x3C;
            response[4] = rotation_rate >> 8;
            response[5] = rotation_rate;
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }

        FURI_LOG_W(TAG, "Unsupported VPD code %02X", page_code);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
        return false;
    }; break;
    case SCSI_READ_FORMAT_CAPACITIES: {
        FURI_LOG_D(TAG, "SCSI_READ_FORMAT_CAPACITIES");
        if(cap < 12) {
            return false;
        }
        uint32_t n_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
        uint32_t block_size = scsi->fn.block_size;
        // Capacity List Header
        data[0] = 0;
        data[1] = 0;
        data[2] = 0;
        data[3] = 8;

        // Capacity Descriptor
        data[4] = n_blocks >> 24;
        data[5] = n_blocks >> 16;
        data[6] = n_blocks >> 8;
        data[7] = n_blocks & 0xFF;
        data[8] = 0x02; // Formatted media
        data[9] = block_size >> 16;
        data[10] = block_size >> 8;
        data[11] = block_size & 0xFF;
        *len = 12;
        scsi->tx_done = true;
        return true;
    }; break;
    case SCSI_READ_CAPACITY_10: {
        FURI_LOG_D(TAG, "SCSI_READ_CAPACITY_10");
        if(cap < 8) return false;
        uint32_t n_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
        uint32_t block_size = scsi->fn.block_size;
        data[0] = (n_blocks - 1) >> 24;
        data[1] = (n_blocks - 1) >> 16;
        data[2] = (n_blocks - 1) >> 8;
        data[3] = (n_blocks - 1) & 0xFF;
        data[4] = block_size >> 24;
        data[5] = block_size >> 16;
        data[6] = block_size >> 8;
        data[7] = block_size & 0xFF;
        *len = 8;
        scsi->tx_done = true;
        return true;
    }; break;
    case SCSI_MODE_SENSE_6: {
        FURI_LOG_D(TAG, "SCSI_MODE_SENSE_6 %lu", cap);
        return scsi_mode_sense(scsi, data, len, cap, false);
    }; break;
    case SCSI_MODE_SENSE_10: {
        FURI_LOG_D(TAG, "SCSI_MODE_SENSE_10");
        return scsi_mode_sense(scsi, data, len, cap, true);
    }; break;
    case SCSI_READ_TOC: {
        FURI_LOG_D(TAG, "SCSI_READ_TOC");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }

        uint8_t format = scsi->cmd[2] & 0x0F;
        if(format == 0) format = scsi->cmd[9] >> 6;
        if(format == 4) {
            uint8_t response[28] = {
                0x00,
                0x1A,
                0x00,
                0x00,
                0xA0,
                0x00,
                0xB8,
                0x00,
                0x61,
                0x22,
                0x17,
                0x00,
            };
            scsi_store_cdrom_address(response + 11, scsi->fn.num_blocks(scsi->fn.ctx), true);
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }
        if(format > 1 || (!scsi->fn.read_only && !scsi->optical_finalized)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }

        uint8_t response[20] = {
            0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        bool msf = scsi->cmd[1] & 0x02;
        scsi_store_cdrom_address(response + 8, 0, msf);
        uint32_t lead_out = scsi->fn.read_only ? scsi->fn.num_blocks(scsi->fn.ctx) :
                                                 scsi->next_writable_lba;
        scsi_store_cdrom_address(response + 16, lead_out, msf);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_HEADER: {
        FURI_LOG_D(TAG, "SCSI_READ_HEADER");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        uint32_t lba = scsi_read_be32(scsi->cmd + 2);
        if(lba >= scsi->fn.num_blocks(scsi->fn.ctx)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_LBA_OOB;
            return false;
        }
        uint8_t response[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
        scsi_store_cdrom_address(response + 4, lba, scsi->cmd[1] & 0x02);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_GET_CONFIGURATION: {
        FURI_LOG_D(TAG, "SCSI_GET_CONFIGURATION");
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        if(scsi->fn.read_only) {
            const uint8_t response[8] = {0, 0, 0, 4, 0, 0, 0, 8};
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }
        const uint8_t features[] = {
            0x00, 0x00, 0x03, 0x08, 0x00, 0x09, 0x01, 0x00, // profile list
            0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0B, 0x08,
            0x00, 0x00, 0x00, 0x00, // core
            0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0B, 0x04,
            0x29, 0x00, 0x00, 0x00, // removable medium
            0x00, 0x10, 0x03, 0x08, 0x00, 0x00, 0x08, 0x00, // random readable
            0x00, 0x01, 0x01, 0x00, 0x00, 0x1E, 0x03, 0x04,
            0x00, 0x00, 0x00, 0x00, // CD read
            0x00, 0x2D, 0x09, 0x04, 0x00, 0x00, 0x01, 0x00, // CD TAO
        };
        uint8_t response[68] = {0};
        uint8_t response_len = 8;
        uint8_t request_type = scsi->cmd[1] & 0x03;
        uint16_t starting_feature = scsi->cmd[2] << 8 | scsi->cmd[3];
        if(request_type == 3) return false;
        response[7] = 0x09; // current profile: CD-R
        for(uint8_t offset = 0; offset < sizeof(features);) {
            uint16_t feature = features[offset] << 8 | features[offset + 1];
            uint8_t feature_len = features[offset + 3] + 4;
            bool selected = request_type == 2 ? feature == starting_feature :
                                                feature >= starting_feature;
            if(selected) {
                memcpy(response + response_len, features + offset, feature_len);
                response_len += feature_len;
            }
            offset += feature_len;
        }
        scsi_store_be32(response, response_len - 4);
        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_GET_EVENT_STATUS: {
        FURI_LOG_D(TAG, "SCSI_GET_EVENT_STATUS");
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        const uint8_t response[8] = {0, 6, 4, 0x10, 0, 2, 0, 0};
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_DISC_INFORMATION: {
        FURI_LOG_D(TAG, "SCSI_READ_DISC_INFORMATION");
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        uint8_t response[34] = {0};
        response[1] = 0x20;
        response[2] = scsi->fn.read_only || scsi->optical_finalized ? 0x0E :
                      scsi->optical_open                            ? 0x05 :
                                                                      0x00;
        response[3] = 0x01;
        response[4] = 0x01;
        response[5] = 0x01;
        response[6] = 0x01;
        scsi_store_cdrom_address(response + 20, scsi->fn.num_blocks(scsi->fn.ctx), true);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_TRACK_INFORMATION: {
        FURI_LOG_D(TAG, "SCSI_READ_TRACK_INFORMATION");
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical || scsi->cmd_len < 10)
            return false;
        uint8_t address_type = scsi->cmd[1] & 0x03;
        uint32_t address = scsi_read_be32(scsi->cmd + 2);
        if(address_type != 1 || (address != 1 && address != 0xFF)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        uint8_t response[48] = {0};
        uint32_t num_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
        bool writable = !scsi->fn.read_only && !scsi->optical_finalized;
        uint32_t recorded_blocks = scsi->fn.read_only ? num_blocks : scsi->next_writable_lba;
        response[1] = 0x2E;
        response[2] = 0x01;
        response[3] = 0x01;
        response[5] = 0x04;
        response[6] = recorded_blocks ? 0x01 : 0x41;
        response[7] = (writable ? 0x01 : 0x00) | (recorded_blocks ? 0x02 : 0x00);
        if(writable) {
            scsi_store_be32(response + 12, scsi->next_writable_lba);
            scsi_store_be32(response + 16, num_blocks - scsi->next_writable_lba);
        }
        scsi_store_be32(response + 24, writable ? num_blocks : recorded_blocks);
        if(recorded_blocks) scsi_store_be32(response + 28, recorded_blocks - 1);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_10: {
    case SCSI_READ_12:
        uint32_t block_size = scsi->fn.block_size;
        bool result =
            scsi->fn.read(scsi->fn.ctx, scsi->read.lba, scsi->read.count, data, len, cap);
        *len -= *len % block_size;
        uint32_t blocks = *len / block_size;
        scsi->read.lba += blocks;
        scsi->read.count -= blocks;
        if(!scsi->read.count) {
            scsi->tx_done = true;
        }
        return result;
    }; break;
    default: {
        FURI_LOG_W(TAG, "unexpected scsi tx data cmd=%02X", scsi->cmd[0]);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }; break;
    }
}

bool scsi_cmd_end(SCSISession* scsi) {
    FURI_LOG_T(TAG, "END %02X", scsi->cmd[0]);
    uint8_t* cmd = scsi->cmd;
    uint8_t len = scsi->cmd_len;
    scsi->cmd = NULL;
    scsi->cmd_len = 0;
    switch(cmd[0]) {
    case SCSI_WRITE_10:
    case SCSI_WRITE_12:
    case SCSI_MODE_SELECT_10:
        return scsi->rx_done;

    case SCSI_REQUEST_SENSE:
    case SCSI_INQUIRY:
    case SCSI_READ_FORMAT_CAPACITIES:
    case SCSI_READ_CAPACITY_10:
    case SCSI_MODE_SENSE_6:
    case SCSI_MODE_SENSE_10:
    case SCSI_READ_TOC:
    case SCSI_READ_HEADER:
    case SCSI_GET_CONFIGURATION:
    case SCSI_GET_EVENT_STATUS:
    case SCSI_READ_DISC_INFORMATION:
    case SCSI_READ_TRACK_INFORMATION:
    case SCSI_READ_10:
    case SCSI_READ_12:
        return scsi->tx_done;

    case SCSI_TEST_UNIT_READY: {
        FURI_LOG_D(TAG, "SCSI_TEST_UNIT_READY");
        return true;
    }; break;
    case SCSI_PREVENT_MEDIUM_REMOVAL: {
        if(len < 6) return false;
        bool prevent = cmd[4] & 1;
        FURI_LOG_D(TAG, "SCSI_PREVENT_MEDIUM_REMOVAL prevent=%d", prevent);
        return true;
    }; break;
    case SCSI_START_STOP_UNIT: {
        if(len < 6) return false;
        bool eject = (cmd[4] & 2) != 0;
        bool start = (cmd[4] & 1) != 0;
        FURI_LOG_D(TAG, "SCSI_START_STOP_UNIT eject=%d start=%d", eject, start);
        if(eject && !start) {
            scsi->fn.eject(scsi->fn.ctx);
        }
        return true;
    }; break;
    case SCSI_VERIFY_10: {
        if(len < 10 || (cmd[1] & 0x02)) return false;
        FURI_LOG_D(TAG, "SCSI_VERIFY_10");
        return true;
    }; break;
    case SCSI_SYNCHRONIZE_CACHE_10: {
        FURI_LOG_D(TAG, "SCSI_SYNCHRONIZE_CACHE_10");
        return scsi->fn.sync(scsi->fn.ctx);
    }; break;
    case SCSI_RESERVE_TRACK: {
        if(len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical || (cmd[1] & 1)) {
            return false;
        }
        uint32_t blocks = scsi_read_be32(cmd + 5);
        uint32_t remaining = scsi->fn.num_blocks(scsi->fn.ctx) - scsi->next_writable_lba;
        if(blocks > remaining) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_LBA_OOB;
            return false;
        }
        scsi->reserved_blocks = blocks;
        FURI_LOG_D(TAG, "SCSI_RESERVE_TRACK %08lX", blocks);
        return true;
    }; break;
    case SCSI_CLOSE_TRACK_SESSION: {
        if(len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        uint8_t close_function = cmd[2] & 0x07;
        if(close_function != 1 && close_function != 2) return false;
        FURI_LOG_D(TAG, "SCSI_CLOSE_TRACK_SESSION function=%u", close_function);
        if(!scsi->fn.sync(scsi->fn.ctx)) return false;
        if(close_function == 2) scsi->optical_finalized = true;
        return true;
    }; break;
    case SCSI_SET_CD_SPEED: {
        FURI_LOG_D(TAG, "SCSI_SET_CD_SPEED");
        return scsi->fn.device_type == MassStorageDeviceTypeOptical;
    }; break;
    default: {
        FURI_LOG_W(TAG, "unexpected scsi cmd=%02X", cmd[0]);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }; break;
    }
}
