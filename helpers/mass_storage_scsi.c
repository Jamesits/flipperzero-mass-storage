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
#define SCSI_MODE_SENSE_10          (0x5A)
#define SCSI_READ_12                (0xA8)
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

static bool
    scsi_mode_sense(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap, bool ten_byte) {
    uint8_t response[20] = {0};
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
        if(page_control == 3 || (page_code != 0x08 && page_code != 0x3F)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }

        uint8_t* page = response + header_len;
        page[0] = 0x08; // caching page
        page[1] = 10;
        if(page_control != 1) {
            page[2] = 0x04; // write cache enabled, read cache enabled
            page[4] = 0xFF;
            page[5] = 0xFF;
            page[8] = 0xFF;
            page[9] = 0xFF;
            page[10] = 0xFF;
            page[11] = 0xFF;
        }
        response_len += 12;
    }

    if(ten_byte) {
        response[0] = (response_len - 2) >> 8;
        response[1] = response_len - 2;
    } else {
        response[0] = response_len - 1;
    }
    return scsi_tx_response(scsi, data, len, cap, response, response_len);
}

bool scsi_cmd_start(SCSISession* scsi, uint8_t* cmd, uint8_t len) {
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
        if(scsi->fn.read_only) {
            scsi->sk = SCSI_SK_DATA_PROTECT;
            scsi->asc = SCSI_ASC_WRITE_PROTECTED;
            return false;
        }
        scsi->write_10.lba = scsi_read_be32(cmd + 2);
        scsi->write_10.count = cmd[7] << 8 | cmd[8];
        FURI_LOG_D(TAG, "SCSI_WRITE_10 %08lX %04X", scsi->write_10.lba, scsi->write_10.count);
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
    case SCSI_WRITE_10: {
        uint32_t block_size = scsi->fn.block_size;
        uint16_t blocks = len / block_size;
        bool result =
            scsi->fn.write(scsi->fn.ctx, scsi->write_10.lba, blocks, data, blocks * block_size);
        scsi->write_10.lba += blocks;
        scsi->write_10.count -= blocks;
        if(!scsi->write_10.count) {
            scsi->rx_done = true;
        }
        return result;
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
        if(format > 1) return false;

        uint8_t response[20] = {
            0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        bool msf = scsi->cmd[1] & 0x02;
        scsi_store_cdrom_address(response + 8, 0, msf);
        scsi_store_cdrom_address(response + 16, scsi->fn.num_blocks(scsi->fn.ctx), msf);
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
        const uint8_t response[8] = {0, 0, 0, 4, 0, 0, 0, 8}; // current profile: CD-ROM
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
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
        const uint8_t response[34] = {
            0x00, 0x20, 0x0E, 0x01, 0x01, 0x01, 0x01, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
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
        return true;
    }; break;
    default: {
        FURI_LOG_W(TAG, "unexpected scsi cmd=%02X", cmd[0]);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }; break;
    }
}
