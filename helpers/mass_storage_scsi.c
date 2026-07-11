#include "mass_storage_scsi.h"

#include <core/log.h>
#include <furi_hal_version.h>

#define TAG "MassStorageSCSI"

#define PERFORMANCE_START 250 // KB/s
#define PERFORMANCE_END   250 // KB/s

#define CD_RW_PACKET_SIZE (32UL)

#define ULTRAISO_DISC_INFO_ALLOCATION (0x0800)

#define SCSI_TEST_UNIT_READY        (0x00)
#define SCSI_REZERO_UNIT            (0x01)
#define SCSI_REQUEST_SENSE          (0x03)
#define SCSI_FORMAT_UNIT            (0x04)
#define SCSI_INQUIRY                (0x12)
#define SCSI_MODE_SENSE_6           (0x1A)
#define SCSI_START_STOP_UNIT        (0x1B)
#define SCSI_SEND_DIAGNOSTIC        (0x1D)
#define SCSI_PREVENT_MEDIUM_REMOVAL (0x1E)
#define SCSI_READ_FORMAT_CAPACITIES (0x23)
#define SCSI_READ_CAPACITY_10       (0x25)
#define SCSI_READ_10                (0x28)
#define SCSI_WRITE_10               (0x2A)
#define SCSI_SEEK_10                (0x2B)
#define SCSI_VERIFY_10              (0x2F)
#define SCSI_SYNCHRONIZE_CACHE_10   (0x35)
#define SCSI_READ_SUB_CHANNEL       (0x42)
#define SCSI_READ_TOC               (0x43)
#define SCSI_READ_HEADER            (0x44)
#define SCSI_PLAY_AUDIO_10          (0x45)
#define SCSI_GET_CONFIGURATION      (0x46)
#define SCSI_PLAY_AUDIO_MSF         (0x47)
#define SCSI_PLAY_AUDIO_TRACK_INDEX (0x48)
#define SCSI_GET_EVENT_STATUS       (0x4A)
#define SCSI_PAUSE_RESUME           (0x4B)
#define SCSI_STOP_PLAY_SCAN         (0x4E)
#define SCSI_READ_DISC_INFORMATION  (0x51)
#define SCSI_READ_TRACK_INFORMATION (0x52)
#define SCSI_RESERVE_TRACK          (0x53)
#define SCSI_MODE_SELECT_10         (0x55)
#define SCSI_MODE_SENSE_10          (0x5A)
#define SCSI_CLOSE_TRACK_SESSION    (0x5B)
#define SCSI_READ_BUFFER_CAPACITY   (0x5C)
#define SCSI_SEND_CUE_SHEET         (0x5D)
#define SCSI_BLANK                  (0xA1)
#define SCSI_PLAY_AUDIO_12          (0xA5)
#define SCSI_READ_12                (0xA8)
#define SCSI_WRITE_12               (0xAA)
#define SCSI_GET_PERFORMANCE        (0xAC)
#define SCSI_READ_CD_MSF            (0xB9)
#define SCSI_SCAN                   (0xBA)
#define SCSI_SET_CD_SPEED           (0xBB)
#define SCSI_MECHANISM_STATUS       (0xBD)
#define SCSI_READ_CD                (0xBE)

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

bool scsi_is_usb_disk(MassStorageDeviceType device_type) {
    return device_type == MassStorageDeviceTypeUsbSsd ||
           device_type == MassStorageDeviceTypeUsbHdd;
}

void scsi_session_init(SCSISession* scsi, SCSIDeviceFunc fn) {
    memset(scsi, 0, sizeof(SCSISession));
    scsi->fn = fn;

    if(fn.device_type == MassStorageDeviceTypeOptical && fn.optical_formatted) {
        uint32_t blocks = fn.num_blocks(fn.ctx);
        scsi->formatted_blocks = blocks - blocks % CD_RW_PACKET_SIZE;
        scsi->optical_packet_size = CD_RW_PACKET_SIZE;
        scsi->optical_formatted = true;
    }
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

static void scsi_store_cdrom_relative_address(uint8_t* data, uint32_t lba, bool msf) {
    if(msf) {
        data[0] = 0;
        data[1] = lba / (60 * 75);
        data[2] = (lba / 75) % 60;
        data[3] = lba % 75;
    } else {
        scsi_store_cdrom_address(data, lba, false);
    }
}

static bool scsi_cdrom_address_to_lba(const uint8_t* msf, uint32_t* lba) {
    if(msf[1] >= 60 || msf[2] >= 75) return false;
    uint32_t address = ((uint32_t)msf[0] * 60 + msf[1]) * 75 + msf[2];
    if(address < 150) return false;
    *lba = address - 150;
    return true;
}

static uint32_t scsi_read_be32(const uint8_t* data) {
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static uint32_t scsi_read_be24(const uint8_t* data) {
    return (uint32_t)data[0] << 16 | (uint32_t)data[1] << 8 | data[2];
}

static void scsi_store_be32(uint8_t* data, uint32_t value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static void scsi_store_be24(uint8_t* data, uint32_t value) {
    data[0] = value >> 16;
    data[1] = value >> 8;
    data[2] = value;
}

static uint32_t scsi_medium_blocks(SCSISession* scsi) {
    if(scsi->fn.device_type == MassStorageDeviceTypeOptical && scsi->optical_formatted &&
       scsi->formatted_blocks) {
        return scsi->formatted_blocks;
    }
    return scsi->fn.num_blocks(scsi->fn.ctx);
}

static void scsi_set_sense(SCSISession* scsi, uint8_t sk, uint8_t asc, uint8_t ascq);

static uint8_t scsi_audio_track_count(SCSISession* scsi) {
    if(!scsi->fn.audio_cd || !scsi->fn.audio_track_count) return 0;
    return scsi->fn.audio_track_count(scsi->fn.ctx);
}

static bool scsi_audio_track_info(SCSISession* scsi, uint8_t track, SCSIAudioTrackInfo* info) {
    return scsi->fn.audio_cd && scsi->fn.audio_track_info &&
           scsi->fn.audio_track_info(scsi->fn.ctx, track, info);
}

static bool scsi_audio_track_for_lba(
    SCSISession* scsi,
    uint32_t lba,
    SCSIAudioTrackInfo* info,
    uint8_t* index) {
    uint8_t track_count = scsi_audio_track_count(scsi);
    for(uint8_t track = track_count; track > 0; track--) {
        if(!scsi_audio_track_info(scsi, track, info)) return false;
        uint32_t boundary = info->has_index0 ? info->index0_lba : info->start_lba;
        if(lba >= boundary) {
            *index = info->has_index0 && lba < info->start_lba ? 0 : 1;
            return true;
        }
    }
    return false;
}

static bool scsi_audio_control(
    SCSISession* scsi,
    SCSIAudioControl control,
    uint32_t start_lba,
    uint32_t end_lba) {
    if(!scsi->fn.audio_cd || !scsi->fn.audio_control ||
       !scsi->fn.audio_control(scsi->fn.ctx, control, start_lba, end_lba)) {
        scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_ILLEGAL_MODE_FOR_TRACK, 0);
        return false;
    }
    return true;
}

static uint32_t scsi_optical_packet_size(SCSISession* scsi) {
    return scsi->optical_packet_size ? scsi->optical_packet_size : CD_RW_PACKET_SIZE;
}

static void scsi_set_sense(SCSISession* scsi, uint8_t sk, uint8_t asc, uint8_t ascq) {
    scsi->sk = sk;
    scsi->asc = asc;
    scsi->ascq = ascq;
}

static bool scsi_is_blank_cancel_command(const uint8_t* cmd, uint8_t len) {
    return len >= 6 && cmd[0] == SCSI_START_STOP_UNIT && cmd[1] == 0x01 && cmd[2] == 0 &&
           cmd[3] == 0 && cmd[4] == 0;
}

static void scsi_set_blank_progress(SCSISession* scsi, uint16_t progress, bool active) {
    scsi->blank.progress = progress;
    scsi->blank.active = active;
    if(scsi->fn.wipe_progress) {
        scsi->fn.wipe_progress(scsi->fn.ctx, progress, active);
    }
}

static void scsi_finish_blank(SCSISession* scsi, bool success) {
    scsi->blank.operational_change_pending = true;
    scsi->blank.busy_change_pending = true;
    if(success) {
        scsi->next_writable_lba = 0;
        scsi->reserved_blocks = 0;
        scsi->formatted_blocks = 0;
        scsi->optical_packet_size = 0;
        scsi->optical_open = false;
        scsi->optical_finalized = false;
        scsi->optical_formatted = false;
        scsi->blank.failed = false;
        scsi_set_sense(scsi, 0, 0, 0);
        scsi_set_blank_progress(scsi, UINT16_MAX, false);
    } else {
        scsi->blank.failed = true;
        scsi_set_sense(scsi, SCSI_SK_MEDIUM_ERROR, SCSI_ASC_WRITE_ERROR, 0);
        scsi_set_blank_progress(scsi, scsi->blank.progress, false);
    }
}

static void scsi_start_blank(SCSISession* scsi, bool immediate) {
    scsi->blank.lba = 0;
    scsi->blank.total_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
    scsi->blank.failed = false;
    scsi->blank.immediate = immediate;
    scsi->blank.operational_change_pending = true;
    scsi->blank.busy_change_pending = true;
    scsi_set_sense(scsi, 0, 0, 0);
    scsi_set_blank_progress(scsi, 0, true);
}

bool scsi_blank_in_progress(const SCSISession* scsi) {
    return scsi->blank.active;
}

bool scsi_blank_defers_status(const SCSISession* scsi) {
    return scsi->blank.active && !scsi->blank.immediate;
}

bool scsi_blank_succeeded(const SCSISession* scsi) {
    return !scsi->blank.failed;
}

void scsi_blank_step(SCSISession* scsi, uint8_t* buffer, uint32_t buffer_size) {
    if(!scsi->blank.active) return;

    uint32_t block_size = scsi->fn.block_size;
    uint32_t buffer_blocks = block_size ? buffer_size / block_size : 0;
    uint32_t remaining = scsi->blank.total_blocks - scsi->blank.lba;
    if(remaining) {
        if(!buffer || !buffer_blocks) {
            scsi_finish_blank(scsi, false);
            return;
        }

        uint16_t blocks = MIN(remaining, MIN(buffer_blocks, (uint32_t)UINT16_MAX));
        uint32_t write_size = blocks * block_size;
        memset(buffer, 0, write_size);
        if(!scsi->fn.write(scsi->fn.ctx, scsi->blank.lba, blocks, buffer, write_size)) {
            scsi_finish_blank(scsi, false);
            return;
        }

        scsi->blank.lba += blocks;
        uint16_t progress = scsi->blank.total_blocks ?
                                (uint64_t)scsi->blank.lba * UINT16_MAX / scsi->blank.total_blocks :
                                UINT16_MAX;
        if(!progress) progress = 1;
        if(progress != scsi->blank.progress) {
            scsi_set_blank_progress(scsi, progress, true);
        }
    }

    if(scsi->blank.lba == scsi->blank.total_blocks) {
        scsi_finish_blank(scsi, scsi->fn.sync(scsi->fn.ctx));
    }
}

void scsi_blank_cancel(SCSISession* scsi) {
    if(!scsi->blank.active) return;
    scsi->blank.operational_change_pending = true;
    scsi->blank.busy_change_pending = true;
    scsi->blank.failed = false;
    scsi_set_sense(scsi, 0, 0, 0);
    scsi_set_blank_progress(scsi, scsi->blank.progress, false);
}

static uint16_t scsi_blank_estimated_100ms(const SCSISession* scsi) {
    if(!scsi->blank.active) return 0;

    uint64_t remaining_bytes =
        (uint64_t)(scsi->blank.total_blocks - scsi->blank.lba) * scsi->fn.block_size;
    uint64_t units =
        (remaining_bytes * 10 + PERFORMANCE_END * 1000 - 1) / (PERFORMANCE_END * 1000);
    if(units < 2) return 2;
    if(units > UINT16_MAX) return UINT16_MAX;
    return units;
}

static uint8_t
    scsi_mode_page(SCSISession* scsi, uint8_t* page, uint8_t page_code, uint8_t page_control) {
    bool read_only = scsi->fn.read_only;
    if(page_code == 0x01) {
        page[0] = 0x01;
        page[1] = 10;
        return 12;
    } else if(page_code == 0x05) {
        page[0] = 0x05;
        page[1] = 50;
        if(page_control == 1) {
            page[2] = 0x2F;
            page[3] = 0xFF;
            page[4] = 0x0F;
            page[5] = 0xFF;
            page[7] = 0x3F;
            page[8] = 0xFF;
            memset(page + 10, 0xFF, 6);
        } else {
            page[2] = scsi->optical_formatted ?
                          0x20 : // valid link size, packet/restricted overwrite
                          0x21; // valid link size, track-at-once
            page[3] = scsi->optical_formatted ? 0x24 : // fixed packet, Mode 1 data track
                                                0x04; // Mode 1 data track
            page[4] = 0x08; // 2048-byte Mode 1 user data
            page[5] = 0x07;
            scsi_store_be32(page + 10, scsi_optical_packet_size(scsi));
            page[14] = 0x00;
            page[15] = 0x96;
        }
        return 52;
    } else if(page_code == 0x08) {
        page[0] = 0x08;
        page[1] = 10;
        // Do not advertise a volatile write cache. Reads remain cached.
        return 12;
    } else if(page_code == 0x0E && scsi->fn.audio_cd) {
        page[0] = 0x0E;
        page[1] = 14;
        if(page_control != 1) {
            page[2] = 0x04; // IMMED: report playback commands as soon as accepted
            page[8] = 0x01; // output port 0: left channel
            page[9] = 0xFF;
            page[10] = 0x02; // output port 1: right channel
            page[11] = 0xFF;
        }
        return 16;
    } else if(page_code == 0x2A) {
        page[0] = 0x2A;
        page[1] = 30;
        if(page_control != 1) {
            page[2] = 0x03; // read CD-R and CD-RW
            page[3] = read_only ? 0x00 : 0x03; // write CD-R and CD-RW
            page[4] = 0xC0 | (scsi->fn.audio_cd ? 0x01 : 0x00);
            if(scsi->fn.audio_cd) page[5] = 0x03; // CD-DA commands and accurate streaming
            page[6] = 0x29; // tray, eject and lock supported
            page[8] = 0x01; // 353 KB/s (2x) max read speed
            page[9] = 0x61;
            page[12] = 0x00;
            page[13] = 0x10; // 16 KiB buffer
            page[14] = 0x01; // 353 KB/s (2x) current read speed
            page[15] = 0x61;
            page[18] = 0x01; // 353 KB/s (2x) max write speed
            page[19] = 0x61;
            page[20] = 0x01; // 353 KB/s (2x) current write speed
            page[21] = 0x61;
            page[28] = 0x01;
            page[29] = 0x61;
        }
        return 32;
    }
    return 0;
}

static bool
    scsi_mode_sense(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap, bool ten_byte) {
    uint8_t response[136] = {0};
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

        const uint8_t pages[] = {0x01, 0x05, 0x08, 0x0E, 0x2A};
        for(uint8_t i = 0; i < COUNT_OF(pages); i++) {
            if(page_code == pages[i] || page_code == 0x3F) {
                response_len +=
                    scsi_mode_page(scsi, response + response_len, pages[i], page_control);
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
    uint32_t num_blocks = scsi_medium_blocks(scsi);
    // A formatted CD-RW allows random overwrite anywhere; otherwise recording is
    // sequential and must continue at the next writable address.
    bool sequential = scsi->fn.device_type == MassStorageDeviceTypeOptical &&
                      !scsi->optical_formatted;
    bool bad_lba;
    if(sequential && (int32_t)lba < 0) {
        // Disc-At-Once burns write the track 1 pre-gap/lead-in at negative LBAs
        // (0xFFFFFF6A..0xFFFFFFFF) just before the program area at LBA 0. Allow it while the
        // disc is still empty; the pre-gap blocks are discarded in scsi_cmd_rx_data and the
        // write flows into the program area at LBA 0.
        int64_t end = (int64_t)(int32_t)lba + count; // first LBA past this write
        bad_lba = scsi->optical_finalized || scsi->next_writable_lba != 0 || end > num_blocks;
    } else {
        bad_lba = lba > num_blocks || count > num_blocks - lba;
        if(sequential) {
            bad_lba = bad_lba || scsi->optical_finalized || lba != scsi->next_writable_lba;
        }
    }
    if(bad_lba) {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_LBA_OOB;
        return false;
    }
    scsi->write.lba = lba;
    scsi->write.count = count;
    scsi->rx_done = count == 0;
    return true;
}

static bool scsi_start_read(
    SCSISession* scsi,
    uint32_t lba,
    uint32_t count,
    uint32_t block_size,
    bool device_to_host) {
    uint32_t blocks = scsi_medium_blocks(scsi);
    if(lba > blocks || count > blocks - lba) {
        scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OOB, 0);
        return false;
    }
    if(count && !device_to_host) {
        scsi->phase_error = true;
        return false;
    }
    scsi->read.lba = lba;
    scsi->read.count = count;
    scsi->read.block_size = block_size;
    scsi->tx_done = count == 0;
    return true;
}

bool scsi_cmd_start(
    SCSISession* scsi,
    uint8_t* cmd,
    uint8_t len,
    uint32_t transfer_len,
    bool device_to_host) {
    scsi->phase_error = false;
    scsi->eject_pending = false;
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

    if(scsi->blank.active && !scsi_is_blank_cancel_command(cmd, len) &&
       cmd[0] != SCSI_TEST_UNIT_READY && cmd[0] != SCSI_REQUEST_SENSE && cmd[0] != SCSI_INQUIRY &&
       cmd[0] != SCSI_GET_CONFIGURATION && cmd[0] != SCSI_GET_EVENT_STATUS &&
       cmd[0] != SCSI_READ_DISC_INFORMATION) {
        scsi_set_sense(
            scsi,
            SCSI_SK_NOT_READY,
            SCSI_ASC_LOGICAL_UNIT_NOT_READY,
            SCSI_ASCQ_OPERATION_IN_PROGRESS);
        return false;
    }

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
        if(len < 10) return false;
        // We expect page-format parameters (PF) and simply ignore the SP (save pages) bit:
        // we don't persist mode pages, but rejecting SP outright (with no sense data) made
        // real burners fail "set write parameters" with "no additional sense information".
        scsi->mode_select.remaining = cmd[7] << 8 | cmd[8];
        if(device_to_host || transfer_len != scsi->mode_select.remaining) {
            scsi->phase_error = true;
            return false;
        }
        scsi->rx_done = scsi->mode_select.remaining == 0;
        return true;
    }; break;
    case SCSI_FORMAT_UNIT: {
        if(len < 6 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        if(scsi->fn.read_only) {
            scsi->sk = SCSI_SK_DATA_PROTECT;
            scsi->asc = SCSI_ASC_WRITE_PROTECTED;
            return false;
        }
        // MMC requires FMTDATA=1, CMPLST=0, and format code 001b.
        if((cmd[1] & 0x1F) != 0x11) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        scsi->format.total = transfer_len;
        scsi->format.remaining = transfer_len;
        memset(scsi->format.parameters, 0, sizeof(scsi->format.parameters));
        if(device_to_host || transfer_len < sizeof(scsi->format.parameters)) {
            scsi->phase_error = true;
            return false;
        }
        scsi->rx_done = false;
        FURI_LOG_D(TAG, "SCSI_FORMAT_UNIT %08lX", scsi->format.remaining);
        return true;
    }; break;
    case SCSI_BLANK: {
        if(len < 12 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        if(scsi->fn.read_only) {
            scsi->sk = SCSI_SK_DATA_PROTECT;
            scsi->asc = SCSI_ASC_WRITE_PROTECTED;
            return false;
        }
        // Full and minimal blank both wipe the complete file-backed medium. Partial blanking
        // cannot be represented faithfully by the single-track emulation.
        uint8_t blank_type = cmd[1] & 0x07;
        if(blank_type > 1) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        if(transfer_len) {
            scsi->phase_error = true;
            return false;
        }
        FURI_LOG_D(TAG, "SCSI_BLANK type=%u immediate=%u", blank_type, !!(cmd[1] & 0x10));
        return true;
    }; break;
    case SCSI_SEND_CUE_SHEET: {
        if(len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        if(scsi->fn.read_only) {
            scsi->sk = SCSI_SK_DATA_PROTECT;
            scsi->asc = SCSI_ASC_WRITE_PROTECTED;
            return false;
        }
        // SEND CUE SHEET (0x5D): the host hands over the disc layout for a Disc-At-Once
        // (SAO) burn in the data-out phase. Recording goes straight to the backing file, so
        // we accept and discard the cue sheet; the sequential WRITE(10)s that follow do the
        // work. Length is a 3-byte field in bytes 6..8.
        scsi->cue.remaining = (uint32_t)cmd[6] << 16 | (uint32_t)cmd[7] << 8 | cmd[8];
        if(device_to_host || transfer_len != scsi->cue.remaining) {
            scsi->phase_error = true;
            return false;
        }
        scsi->rx_done = scsi->cue.remaining == 0;
        FURI_LOG_D(TAG, "SCSI_SEND_CUE_SHEET %08lX", scsi->cue.remaining);
        return true;
    }; break;
    case SCSI_READ_10: {
        if(len < 10) return false;
        uint32_t lba = scsi_read_be32(cmd + 2);
        uint32_t count = cmd[7] << 8 | cmd[8];
        FURI_LOG_D(TAG, "SCSI_READ_10 %08lX %04lX", lba, count);
        if(scsi->fn.audio_cd) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_ILLEGAL_MODE_FOR_TRACK, 0);
            return false;
        }
        return scsi_start_read(scsi, lba, count, scsi->fn.block_size, device_to_host);
    }; break;
    case SCSI_READ_12: {
        if(len < 12) return false;
        uint32_t lba = scsi_read_be32(cmd + 2);
        uint32_t count = scsi_read_be32(cmd + 6);
        FURI_LOG_D(TAG, "SCSI_READ_12 %08lX %08lX", lba, count);
        if(scsi->fn.audio_cd) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_ILLEGAL_MODE_FOR_TRACK, 0);
            return false;
        }
        return scsi_start_read(scsi, lba, count, scsi->fn.block_size, device_to_host);
    }; break;
    case SCSI_READ_CD: {
        if(len < 12 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        // READ CD (0xBE): 3-byte transfer length in blocks (bytes 6..8). Burners use it to
        // verify written data. We only store the 2048-byte user data per sector, which is
        // exactly what a Mode 1 user-data read wants, so serve it via the normal read path.
        uint32_t lba = scsi_read_be32(cmd + 2);
        uint32_t count = (uint32_t)cmd[6] << 16 | (uint32_t)cmd[7] << 8 | cmd[8];
        uint32_t block_size = scsi->fn.block_size;
        if(scsi->fn.audio_cd) {
            uint8_t sector_type = (cmd[1] >> 2) & 0x07;
            if((sector_type != 0 && sector_type != 1) || cmd[9] != 0x10 || cmd[10] != 0) {
                scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
                return false;
            }
            block_size = 2352;
            uint64_t expected = (uint64_t)count * block_size;
            if(expected > UINT32_MAX || transfer_len != expected) {
                scsi->phase_error = true;
                return false;
            }
        }
        FURI_LOG_D(TAG, "SCSI_READ_CD %08lX %08lX", lba, count);
        return scsi_start_read(scsi, lba, count, block_size, device_to_host);
    }; break;
    case SCSI_READ_CD_MSF: {
        if(len < 12 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        uint32_t start_lba;
        uint32_t end_lba;
        if(!scsi_cdrom_address_to_lba(cmd + 3, &start_lba) ||
           !scsi_cdrom_address_to_lba(cmd + 6, &end_lba) || end_lba < start_lba) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        if(end_lba > scsi_medium_blocks(scsi)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_LBA_OOB;
            return false;
        }
        uint32_t count = end_lba - start_lba;
        uint32_t block_size = scsi->fn.block_size;
        if(scsi->fn.audio_cd) {
            uint8_t sector_type = (cmd[1] >> 2) & 0x07;
            if((sector_type != 0 && sector_type != 1) || cmd[9] != 0x10 || cmd[10] != 0) {
                scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
                return false;
            }
            block_size = 2352;
            uint64_t expected = (uint64_t)count * block_size;
            if(expected > UINT32_MAX || transfer_len != expected) {
                scsi->phase_error = true;
                return false;
            }
        }
        FURI_LOG_D(TAG, "SCSI_READ_CD_MSF %08lX %08lX", start_lba, count);
        return scsi_start_read(scsi, start_lba, count, block_size, device_to_host);
    }; break;
    case SCSI_PLAY_AUDIO_10:
    case SCSI_PLAY_AUDIO_MSF:
    case SCSI_PLAY_AUDIO_TRACK_INDEX:
    case SCSI_PAUSE_RESUME:
    case SCSI_STOP_PLAY_SCAN:
    case SCSI_PLAY_AUDIO_12:
    case SCSI_SCAN:
        if(!scsi->fn.audio_cd || transfer_len) {
            if(transfer_len) scsi->phase_error = true;
            return false;
        }
        return true;
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
        // Discard any leading pre-gap blocks written at negative LBAs (DAO lead-in); only
        // the program area at LBA >= 0 is backed by the file.
        uint16_t discard = 0;
        if((int32_t)scsi->write.lba < 0) {
            uint32_t to_zero = (uint32_t) - (int32_t)scsi->write.lba;
            discard = to_zero < blocks ? to_zero : blocks;
        }
        uint16_t stored = blocks - discard;
        if(stored) {
            uint32_t lba = scsi->write.lba + discard;
            if(!scsi->fn.write(
                   scsi->fn.ctx, lba, stored, data + discard * block_size, stored * block_size)) {
                return false;
            }
        }
        scsi->write.lba += blocks;
        scsi->write.count -= blocks;
        // Track the next writable address only for sequential recording of real program
        // data; discarded pre-gap blocks don't advance it (data still begins at LBA 0). A
        // formatted CD-RW is overwritten in place and has no moving write pointer.
        if(stored && scsi->fn.device_type == MassStorageDeviceTypeOptical &&
           !scsi->optical_formatted) {
            scsi->next_writable_lba += stored;
            scsi->optical_open = true;
        }
        if(!scsi->write.count) {
            scsi->rx_done = true;
        }
        return true;
    }; break;
    case SCSI_MODE_SELECT_10: {
        if(len > scsi->mode_select.remaining) return false;
        // Host sends an 8-byte parameter header followed by the Write Parameters page (05h).
        // We don't act on the parameters (data is written straight to the backing file), so
        // accept whatever write type / data block type a burner selects (TAO, SAO/DAO,
        // packet); only sanity-check that this really is the write parameters page.
        if(scsi->mode_select.remaining == (uint16_t)(scsi->cmd[7] << 8 | scsi->cmd[8])) {
            uint8_t page = len >= 9 ? data[8] & 0x3F : 0;
            if(len >= 9 && page != 0x05 && page != 0x0E) {
                scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
                scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
                return false;
            }
        }
        scsi->mode_select.remaining -= len;
        scsi->rx_done = scsi->mode_select.remaining == 0;
        return true;
    }; break;
    case SCSI_FORMAT_UNIT: {
        if(len > scsi->format.remaining) return false;
        uint32_t offset = scsi->format.total - scsi->format.remaining;
        if(offset < sizeof(scsi->format.parameters)) {
            uint32_t copy_len = MIN(len, (uint32_t)sizeof(scsi->format.parameters) - offset);
            memcpy(scsi->format.parameters + offset, data, copy_len);
        }
        scsi->format.remaining -= len;
        scsi->rx_done = scsi->format.remaining == 0;
        return true;
    }; break;
    case SCSI_SEND_CUE_SHEET: {
        // We don't parse the cue sheet; just drain the data-out phase.
        if(len > scsi->cue.remaining) return false;
        scsi->cue.remaining -= len;
        scsi->rx_done = scsi->cue.remaining == 0;
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

static bool scsi_audio_read_toc(
    SCSISession* scsi,
    uint8_t* data,
    uint32_t* len,
    uint32_t cap,
    uint8_t format) {
    uint8_t track_count = scsi_audio_track_count(scsi);
    if(track_count == 0) return false;
    bool msf = scsi->cmd[1] & 0x02;

    if(format == 0) {
        uint8_t first = scsi->cmd[6];
        if(first == 0) first = 1;
        if(first > track_count && first != 0xAA) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
            return false;
        }
        uint8_t descriptor_count = first == 0xAA ? 1 : track_count - first + 2;
        uint32_t response_len = 4 + (uint32_t)descriptor_count * 8;
        uint8_t* response = malloc(response_len);
        if(!response) return false;
        memset(response, 0, response_len);
        response[0] = (response_len - 2) >> 8;
        response[1] = response_len - 2;
        response[2] = 1;
        response[3] = track_count;

        uint8_t descriptor = 0;
        if(first != 0xAA) {
            for(uint8_t track = first; track <= track_count; track++) {
                SCSIAudioTrackInfo info;
                if(!scsi_audio_track_info(scsi, track, &info)) {
                    free(response);
                    return false;
                }
                uint8_t* entry = response + 4 + (uint32_t)descriptor++ * 8;
                entry[1] = 0x10; // ADR 1, audio track
                entry[2] = track;
                scsi_store_cdrom_address(entry + 4, info.start_lba, msf);
            }
        }
        uint8_t* lead_out = response + 4 + (uint32_t)descriptor * 8;
        lead_out[1] = 0x10;
        lead_out[2] = 0xAA;
        scsi_store_cdrom_address(lead_out + 4, scsi_medium_blocks(scsi), msf);
        bool result = scsi_tx_response(scsi, data, len, cap, response, response_len);
        free(response);
        return result;
    } else if(format == 1) {
        SCSIAudioTrackInfo info;
        if(!scsi_audio_track_info(scsi, 1, &info)) return false;
        uint8_t response[12] = {0};
        response[1] = 10;
        response[2] = 1;
        response[3] = 1;
        response[5] = 0x10;
        response[6] = 1;
        scsi_store_cdrom_address(response + 8, info.start_lba, msf);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    } else if(format == 2) {
        uint8_t descriptor_count = track_count + 3;
        uint32_t response_len = 4 + (uint32_t)descriptor_count * 11;
        uint8_t* response = malloc(response_len);
        if(!response) return false;
        memset(response, 0, response_len);
        response[0] = (response_len - 2) >> 8;
        response[1] = response_len - 2;
        response[2] = 1;
        response[3] = 1;

        const uint8_t points[] = {0xA0, 0xA1, 0xA2};
        for(uint8_t i = 0; i < COUNT_OF(points); i++) {
            uint8_t* entry = response + 4 + (uint32_t)i * 11;
            entry[0] = 1;
            entry[1] = 0x10;
            entry[3] = points[i];
        }
        response[12] = 1;
        response[23] = track_count;
        uint8_t address[4];
        scsi_store_cdrom_address(address, scsi_medium_blocks(scsi), true);
        memcpy(response + 34, address + 1, 3);

        for(uint8_t track = 1; track <= track_count; track++) {
            SCSIAudioTrackInfo info;
            if(!scsi_audio_track_info(scsi, track, &info)) {
                free(response);
                return false;
            }
            uint8_t* entry = response + 4 + (uint32_t)(track + 2) * 11;
            entry[0] = 1;
            entry[1] = 0x10;
            entry[3] = track;
            scsi_store_cdrom_address(address, info.start_lba, true);
            memcpy(entry + 8, address + 1, 3);
        }
        bool result = scsi_tx_response(scsi, data, len, cap, response, response_len);
        free(response);
        return result;
    }
    return false;
}

bool scsi_cmd_tx_data(SCSISession* scsi, uint8_t* data, uint32_t* len, uint32_t cap) {
    FURI_LOG_T(TAG, "TX %02X cap %lu", scsi->cmd[0], cap);
    if(scsi->tx_done) return false;
    switch(scsi->cmd[0]) {
    case SCSI_REQUEST_SENSE: {
        FURI_LOG_D(TAG, "SCSI_REQUEST_SENSE");
        if(cap < 18) return false;
        bool blank_active = scsi->blank.active;
        memset(data, 0, cap);
        data[0] = 0x70; // fixed format sense data
        data[1] = 0; // obsolete
        data[2] = blank_active ? SCSI_SK_NOT_READY : scsi->sk; // sense key
        data[3] = 0; // information
        data[4] = 0; // information
        data[5] = 0; // information
        data[6] = 0; // information
        data[7] = 10; // additional sense length (len-8)
        data[8] = 0; // command specific information
        data[9] = 0; // command specific information
        data[10] = 0; // command specific information
        data[11] = 0; // command specific information
        data[12] = blank_active ? SCSI_ASC_LOGICAL_UNIT_NOT_READY : scsi->asc;
        data[13] = blank_active ? SCSI_ASCQ_OPERATION_IN_PROGRESS : scsi->ascq;
        data[14] = 0; // field replaceable unit code
        if(blank_active) {
            data[15] = 0x80; // SKSV: bytes 16..17 contain progress indication
            data[16] = scsi->blank.progress >> 8;
            data[17] = scsi->blank.progress;
        }
        *len = 18;
        if(!blank_active) scsi->blank.failed = false;
        scsi->sk = 0;
        scsi->asc = 0;
        scsi->ascq = 0;
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
            // USB disks default to fixed media, but must advertise removable when the
            // host is expected to eject them (see SCSIDeviceFunc.removable).
            response[1] = scsi->fn.removable ? 0x80 : 0x00;
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
            // Unit Serial Number page: report the Flipper's real UID (STM32 96-bit unique ID)
            // as an ASCII hex string.
            const uint8_t* uid = furi_hal_version_uid();
            size_t uid_size = furi_hal_version_uid_size();
            // Cap the UID so the whole page fits the fixed response buffer and the single-byte
            // page-length field.
            if(uid_size > 16) uid_size = 16;
            uint8_t serial_len = uid_size * 2;
            uint8_t response[4 + 16 * 2] = {
                scsi_peripheral_device_type[scsi->fn.device_type], 0x80, 0x00, serial_len};
            static const char hex[] = "0123456789ABCDEF";
            for(size_t i = 0; i < uid_size; i++) {
                response[4 + i * 2] = hex[uid[i] >> 4];
                response[4 + i * 2 + 1] = hex[uid[i] & 0x0F];
            }
            return scsi_tx_response(scsi, data, len, cap, response, 4 + serial_len);
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
        uint8_t response[28] = {0};
        uint32_t max_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
        uint32_t current_blocks = scsi_medium_blocks(scsi);
        bool formattable = scsi->fn.device_type == MassStorageDeviceTypeOptical &&
                           !scsi->fn.read_only;
        uint8_t response_len = formattable ? sizeof(response) : 12;

        response[3] = response_len - 4;
        scsi_store_be32(response + 4, current_blocks);
        response[8] = formattable && !scsi->optical_formatted ?
                          0x01 : // unformatted or blank media
                          0x02; // formatted media
        scsi_store_be24(response + 9, scsi->fn.block_size);

        if(formattable) {
            // Format 00h is required whenever another formattable descriptor is returned.
            scsi_store_be32(response + 12, max_blocks);
            response[16] = 0x00;
            scsi_store_be24(response + 17, scsi->fn.block_size);

            // Format 10h creates the fixed-packet session used by Windows Live File System.
            uint32_t packet_blocks = max_blocks - max_blocks % CD_RW_PACKET_SIZE;
            scsi_store_be32(response + 20, packet_blocks);
            response[24] = 0x10 << 2;
            scsi_store_be24(response + 25, CD_RW_PACKET_SIZE);
        }

        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_READ_CAPACITY_10: {
        FURI_LOG_D(TAG, "SCSI_READ_CAPACITY_10");
        if(cap < 8) return false;
        uint32_t n_blocks = scsi_medium_blocks(scsi);
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
        if(scsi->fn.audio_cd && format <= 2) {
            return scsi_audio_read_toc(scsi, data, len, cap, format);
        }
        if(format == 4) {
            uint8_t response[28] = {
                0x00,
                0x1A,
                0x00,
                0x00,
                0xA0,
                0x40, // unrestricted use
                0x80,
                0x00,
                0x61,
                0x22,
                0x17,
                0x00,
            };
            // ATIP disc type bit (byte 6, bit 6) identifies erasable CD-RW media. A1/A2/A3
            // validity remains clear because no corresponding parameter blocks are returned.
            if(!scsi->fn.read_only) response[6] |= 0x40;
            scsi_store_cdrom_address(response + 11, scsi->fn.num_blocks(scsi->fn.ctx), true);
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }
        if(format == 1) {
            uint8_t response[12] = {
                0x00, 0x0A, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
            scsi_store_cdrom_address(response + 8, 0, scsi->cmd[1] & 0x02);
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        } else if(format == 2) {
            uint8_t response[48] = {0};
            response[1] = 0x2E;
            response[2] = 0x01;
            response[3] = 0x01;

            const uint8_t points[] = {0xA0, 0xA1, 0xA2, 0x01};
            for(uint8_t i = 0; i < COUNT_OF(points); i++) {
                uint8_t* descriptor = response + 4 + i * 11;
                descriptor[0] = 0x01; // session number
                descriptor[1] = 0x14; // ADR 1, data track
                descriptor[3] = points[i];
            }
            response[12] = 0x01; // A0: first track number
            response[23] = 0x01; // A1: last track number

            uint8_t msf[4];
            scsi_store_cdrom_address(msf, scsi_medium_blocks(scsi), true);
            memcpy(response + 34, msf + 1, 3); // A2: lead-out start
            scsi_store_cdrom_address(msf, 0, true);
            memcpy(response + 45, msf + 1, 3); // track 1 start
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        } else if(format == 3) {
            const uint8_t response[4] = {0x00, 0x02, 0x00, 0x00};
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        } else if(format != 0) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }

        // Report the single data track plus lead-out. A real recorder returns a TOC for
        // blank and appendable CD-RW media too, with the lead-out sitting at the next
        // writable address, so a host can qualify the disc for a fresh burn. Rejecting the
        // command here (as we used to for un-finalized writable media) makes burning tools
        // treat the disc as unusable ("media must be blank or formatted").
        uint8_t response[20] = {
            0x00, 0x12, 0x01, 0x01, 0x00, 0x14, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x14, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        bool msf = scsi->cmd[1] & 0x02;
        scsi_store_cdrom_address(response + 8, 0, msf);
        // The lead-out sits at the last-possible address (the full recordable capacity). On
        // blank or appendable media a lead-out at the NWA (0 when blank) made track 1 a
        // malformed zero-length track (LBA 0..-1), which burning tools read as a bogus data
        // track. Anchoring it at capacity keeps track 1 a valid invisible track.
        scsi_store_cdrom_address(response + 16, scsi_medium_blocks(scsi), msf);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_HEADER: {
        FURI_LOG_D(TAG, "SCSI_READ_HEADER");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        uint32_t lba = scsi_read_be32(scsi->cmd + 2);
        if(scsi->fn.audio_cd) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_ILLEGAL_MODE_FOR_TRACK, 0);
            return false;
        }
        if(lba >= scsi_medium_blocks(scsi)) {
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
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }

        static const uint8_t cdrom_features[] = {
            0x00, 0x00, 0x03, 0x04, 0x00, 0x08, 0x01, 0x00, // profile list
            0x00, 0x01, 0x0B, 0x08, 0x00, 0x00, 0x00, 0x00, // core
            0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0B, 0x04,
            0x29, 0x00, 0x00, 0x00, // removable medium
            0x00, 0x10, 0x03, 0x08, 0x00, 0x00, 0x08, 0x00, // random readable
            0x00, 0x01, 0x01, 0x00, 0x00, 0x1D, 0x03, 0x00, // multi-read
            0x00, 0x1E, 0x0B, 0x04, 0x00, 0x00, 0x00, 0x00, // CD read
        };

        static const uint8_t cdrw_features[] = {
            0x00, 0x00, 0x03, 0x08, 0x00, 0x0A, 0x01, 0x00, // profile list
            0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x0B, 0x08,
            0x00, 0x00, 0x00, 0x00, // core
            0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0B, 0x04,
            0x29, 0x00, 0x00, 0x00, // removable medium
            0x00, 0x10, 0x03, 0x08, 0x00, 0x00, 0x08, 0x00, // random readable
            0x00, 0x01, 0x01, 0x00, 0x00, 0x1D, 0x03, 0x00, // multi-read
            0x00, 0x1E, 0x0B, 0x04, 0x00, 0x00, 0x00, 0x00, // CD read
            0x00, 0x21, 0x0D, 0x08, 0x01, 0x00, 0x07, 0x01,
            0x07, 0x00, 0x00, 0x00, // incremental streaming writable
            0x00, 0x23, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, // formattable
            0x00, 0x26, 0x01, 0x00, // restricted overwrite
            0x00, 0x2D, 0x09, 0x04, 0x42, 0x00, 0x01, 0x00, // CD TAO
            0x00, 0x2E, 0x05, 0x04, 0x62, 0x00, 0xFF, 0xFF, // CD mastering
        };

        static const uint8_t audio_play_feature[] = {
            0x01,
            0x03,
            0x01,
            0x04, // CD Audio External Play
            0x04,
            0x00,
            0x00,
            0x00, // scan supported, fixed output volume
        };

        const uint8_t* features = scsi->fn.read_only ? cdrom_features : cdrw_features;
        uint16_t features_len = scsi->fn.read_only ? sizeof(cdrom_features) :
                                                     sizeof(cdrw_features);
        uint8_t response[120] = {0};
        uint16_t response_len = 8;
        uint8_t request_type = scsi->cmd[1] & 0x03;
        uint16_t starting_feature = scsi->cmd[2] << 8 | scsi->cmd[3];
        if(request_type == 3) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        response[7] = scsi->fn.read_only ? 0x08 : 0x0A;
        for(uint16_t offset = 0; offset < features_len;) {
            uint16_t feature = features[offset] << 8 | features[offset + 1];
            uint8_t feature_len = features[offset + 3] + 4;
            bool selected = request_type == 2 ?
                                feature == starting_feature :
                                feature >= starting_feature &&
                                    (request_type != 1 || (features[offset + 2] & 0x01));
            if(selected) {
                memcpy(response + response_len, features + offset, feature_len);
                response_len += feature_len;
            }
            offset += feature_len;
        }
        if(scsi->fn.audio_cd) {
            uint16_t feature = 0x0103;
            bool selected = request_type == 2 ?
                                feature == starting_feature :
                                feature >= starting_feature &&
                                    (request_type != 1 || (audio_play_feature[2] & 0x01));
            if(selected) {
                memcpy(response + response_len, audio_play_feature, sizeof(audio_play_feature));
                response_len += sizeof(audio_play_feature);
            }
        }
        scsi_store_be32(response, response_len - 4);
        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_GET_EVENT_STATUS: {
        FURI_LOG_D(TAG, "SCSI_GET_EVENT_STATUS");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        uint8_t requested_classes = scsi->cmd[4];
        uint8_t response[8] = {0, 6, 4, 0x52, 0, 2, 0, 0};
        uint8_t response_len = sizeof(response);
        if(scsi->blank.operational_change_pending && (requested_classes & 0x02)) {
            response[2] = 1; // Operational Change event class
            response[4] = 2; // Drive operational state changed
            memset(response + 5, 0, 3);
            scsi->blank.operational_change_pending = false;
        } else if(
            (requested_classes & 0x40) &&
            (scsi->blank.busy_change_pending || scsi->blank.active)) {
            uint16_t ready_time = scsi_blank_estimated_100ms(scsi);
            response[2] = 6; // Device Busy event class
            response[4] = scsi->blank.busy_change_pending ? 1 : 0;
            response[5] = scsi->blank.active ? 1 : 0;
            response[6] = ready_time >> 8;
            response[7] = ready_time;
            scsi->blank.busy_change_pending = false;
        } else if(requested_classes & 0x02) {
            response[2] = 1; // Operational Change event class, no change
            memset(response + 4, 0, 4);
        } else if(requested_classes & 0x10) {
            // Media event class: no change, media present.
        } else if(requested_classes & 0x40) {
            response[2] = 6; // Device Busy event class, no change and not busy
            memset(response + 4, 0, 4);
        } else {
            // No requested event class is available. NEA is set and no descriptor follows.
            response[1] = 2;
            response[2] = 0x80;
            response_len = 4;
        }
        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_READ_DISC_INFORMATION: {
        FURI_LOG_D(TAG, "SCSI_READ_DISC_INFORMATION");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        if(scsi->blank.active) {
            uint16_t allocation_length = scsi->cmd[7] << 8 | scsi->cmd[8];
            if(scsi->blank.immediate && allocation_length == ULTRAISO_DISC_INFO_ALLOCATION) {
                // UltraISO polls with a 2 KiB buffer and decides that blanking completed from
                // its first two bytes. Its buffer can remain stale after CHECK CONDITION on
                // some Windows USB-storage stacks, so overwrite the length while staying busy.
                const uint8_t response[2] = {0};
                return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
            }
            scsi_set_sense(
                scsi,
                SCSI_SK_NOT_READY,
                SCSI_ASC_LOGICAL_UNIT_NOT_READY,
                SCSI_ASCQ_OPERATION_IN_PROGRESS);
            return false;
        }
        if(scsi->blank.failed) return false;
        uint8_t data_type = scsi->cmd[1] & 0x07;
        if(data_type == 1) {
            uint8_t response[12] = {0};
            bool appendable = !scsi->fn.read_only && !scsi->optical_finalized;
            uint8_t track_count = scsi->fn.audio_cd ? scsi_audio_track_count(scsi) : 1;
            response[1] = 10;
            response[2] = 0x20; // track resources information
            response[5] = 99; // maximum possible tracks
            response[7] = track_count;
            response[9] = 1; // one track may be open concurrently
            response[11] = appendable ? 1 : 0;
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        } else if(data_type != 0) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }

        uint8_t response[34] = {0};
        response[1] = 0x20;
        // A formatted CD-RW is appendable (in use), an open track is appendable too, and a
        // blank medium is empty. Read-only or finalized media reports complete.
        response[2] = scsi->fn.read_only || scsi->optical_finalized ? 0x0E :
                      scsi->optical_open || scsi->optical_formatted ? 0x05 :
                                                                      0x00;
        // Erasable bit: rewritable CD-RW media can be blanked and rewritten.
        if(!scsi->fn.read_only) response[2] |= 0x10;
        response[3] = 0x01;
        response[4] = 0x01;
        response[5] = 0x01;
        response[6] = scsi->fn.audio_cd ? scsi_audio_track_count(scsi) : 1;
        response[7] = 0x20; // unrestricted-use medium
        response[8] =
            !scsi->fn.read_only && !scsi->optical_open && !scsi->optical_formatted ? 0xFF : 0;
        if(scsi->fn.read_only || scsi->optical_finalized) {
            memset(response + 16, 0xFF, 8);
        } else {
            const uint8_t lead_in[4] = {0x00, 0x61, 0x22, 0x17};
            memcpy(response + 16, lead_in, sizeof(lead_in));
            scsi_store_cdrom_address(response + 20, scsi->fn.num_blocks(scsi->fn.ctx), true);
        }
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_TRACK_INFORMATION: {
        FURI_LOG_D(TAG, "SCSI_READ_TRACK_INFORMATION");
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical || scsi->cmd_len < 10)
            return false;
        uint8_t address_type = scsi->cmd[1] & 0x03;
        uint32_t address = scsi_read_be32(scsi->cmd + 2);
        uint32_t num_blocks = scsi_medium_blocks(scsi);
        if(scsi->fn.audio_cd) {
            SCSIAudioTrackInfo info;
            uint8_t index;
            bool valid_address = false;
            if(address_type == 0 && address < num_blocks) {
                valid_address = scsi_audio_track_for_lba(scsi, address, &info, &index);
            } else if(address_type == 1) {
                if(address <= UINT8_MAX) {
                    uint8_t track = address == 0xFF ? scsi_audio_track_count(scsi) : address;
                    valid_address = scsi_audio_track_info(scsi, track, &info);
                }
            } else if(address_type == 2 && address == 1) {
                valid_address = scsi_audio_track_info(scsi, 1, &info);
            }
            if(!valid_address) {
                scsi_set_sense(
                    scsi,
                    SCSI_SK_ILLEGAL_REQUEST,
                    address_type == 0 ? SCSI_ASC_LBA_OOB : SCSI_ASC_INVALID_FIELD_IN_CDB,
                    0);
                return false;
            }

            uint8_t response[48] = {0};
            response[1] = 0x2E;
            response[2] = info.number;
            response[3] = 1;
            scsi_store_be32(response + 8, info.start_lba);
            memset(response + 12, 0xFF, 8);
            scsi_store_be32(response + 24, info.end_lba - info.start_lba);
            if(info.end_lba > info.start_lba) {
                scsi_store_be32(response + 28, info.end_lba - 1);
            }
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }
        bool valid_address = (address_type == 0 && address < num_blocks) ||
                             (address_type == 1 && (address == 1 || address == 0xFF)) ||
                             (address_type == 2 && address == 1);
        if(!valid_address) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = address_type == 0 ? SCSI_ASC_LBA_OOB : SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
        uint8_t response[48] = {0};
        if((scsi->cmd[1] & 0x04) && (scsi->fn.read_only || scsi->optical_finalized)) {
            response[1] = 0x2E;
            response[2] = 0xFF;
            response[3] = 0xFF;
            response[32] = 0xFF;
            response[33] = 0xFF;
            return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
        }

        bool writable = !scsi->fn.read_only && !scsi->optical_finalized;
        uint32_t recorded_blocks = scsi->fn.read_only ? num_blocks : scsi->next_writable_lba;
        response[1] = 0x2E;
        response[2] = 0x01;
        response[3] = 0x01;
        response[5] = 0x04;
        if(scsi->optical_formatted) {
            response[6] = 0x31; // Mode 1, fixed-packet restricted overwrite
            response[7] = writable ? 0x01 : 0x00;
            if(writable) {
                scsi_store_be32(response + 12, 0);
                scsi_store_be32(response + 16, num_blocks);
            }
            scsi_store_be32(response + 20, scsi_optical_packet_size(scsi));
        } else {
            response[6] = recorded_blocks ? 0x01 : 0x41;
            response[7] = (writable ? 0x01 : 0x00) | (recorded_blocks ? 0x02 : 0x00);
            if(writable) {
                scsi_store_be32(response + 12, scsi->next_writable_lba);
                scsi_store_be32(response + 16, num_blocks - scsi->next_writable_lba);
            }
            if(recorded_blocks) scsi_store_be32(response + 28, recorded_blocks - 1);
        }
        scsi_store_be32(response + 24, writable ? num_blocks : recorded_blocks);
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_READ_10: {
    case SCSI_READ_12:
    case SCSI_READ_CD:
    case SCSI_READ_CD_MSF:
        uint32_t block_size = scsi->read.block_size;
        bool result =
            scsi->fn.read(scsi->fn.ctx, scsi->read.lba, scsi->read.count, data, len, cap);
        *len -= *len % block_size;
        if(!result || (!*len && scsi->read.count)) {
            scsi_set_sense(scsi, SCSI_SK_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ_ERROR, 0);
            return false;
        }
        uint32_t blocks = *len / block_size;
        scsi->read.lba += blocks;
        scsi->read.count -= blocks;
        if(!scsi->read.count) {
            scsi->tx_done = true;
        }
        return true;
    }; break;
    case SCSI_READ_SUB_CHANNEL: {
        FURI_LOG_D(TAG, "SCSI_READ_SUB_CHANNEL");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        bool msf = scsi->cmd[1] & 0x02;
        bool subq = scsi->cmd[2] & 0x40;
        uint8_t format = scsi->cmd[3];
        uint8_t response[24] = {0};
        uint8_t response_len = 4;
        SCSIAudioStatus audio_status = {.status = SCSIAudioStatusNone};
        if(scsi->fn.audio_cd && scsi->fn.audio_get_status &&
           !scsi->fn.audio_get_status(scsi->fn.ctx, &audio_status)) {
            return false;
        }
        response[1] = audio_status.status;
        if(subq) {
            if(format == 0x01) {
                response[4] = 0x01;
                if(scsi->fn.audio_cd) {
                    SCSIAudioTrackInfo info;
                    if(!scsi_audio_track_info(scsi, audio_status.track, &info)) return false;
                    uint32_t relative =
                        audio_status.lba >= info.start_lba ? audio_status.lba - info.start_lba : 0;
                    response[5] = 0x10;
                    response[6] = audio_status.track;
                    response[7] = audio_status.index;
                    scsi_store_cdrom_address(response + 8, audio_status.lba, msf);
                    scsi_store_cdrom_relative_address(response + 12, relative, msf);
                } else {
                    response[5] = 0x14;
                    response[6] = 0x01;
                    response[7] = 0x01;
                    scsi_store_cdrom_address(response + 8, 0, msf);
                    scsi_store_cdrom_relative_address(response + 12, 0, msf);
                }
                response_len = 16;
            } else if(format == 0x02 || format == 0x03) {
                // Media catalog number / track ISRC: none recorded, so leave the valid bit
                // (byte 8, bit 7) clear and return the zeroed identifier fields.
                response[4] = format;
                response_len = 24;
            } else {
                scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
                scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
                return false;
            }
        }
        response[2] = (response_len - 4) >> 8;
        response[3] = response_len - 4;
        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_READ_BUFFER_CAPACITY: {
        FURI_LOG_D(TAG, "SCSI_READ_BUFFER_CAPACITY");
        if(scsi->cmd_len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        // Writes are consumed straight into the backing file, so the recording buffer is
        // effectively always empty. Advertise a fixed buffer that is entirely available so a
        // burner never throttles waiting for buffer space. The Block bit (byte 1 bit 0)
        // selects whether the lengths are counted in blocks or bytes.
        bool in_blocks = scsi->cmd[1] & 0x01;
        uint32_t buffer_bytes = 0x40000; // 256 KiB
        uint32_t buffer = in_blocks ? buffer_bytes / scsi->fn.block_size : buffer_bytes;
        uint8_t response[12] = {0};
        response[1] = 0x0A; // data length (10)
        response[3] = in_blocks ? 0x01 : 0x00;
        scsi_store_be32(response + 4, buffer); // total buffer length
        scsi_store_be32(response + 8, buffer); // blank (available) buffer length
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    case SCSI_GET_PERFORMANCE: {
        FURI_LOG_D(TAG, "SCSI_GET_PERFORMANCE");
        if(scsi->cmd_len < 12 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        uint8_t type = scsi->cmd[10];
        uint16_t max_desc = scsi->cmd[8] << 8 | scsi->cmd[9];
        uint8_t response[24] = {0};
        uint8_t response_len = 8; // performance data header
        // Only nominal performance (type 0) is described. The host caps the descriptor count
        // in bytes 8..9; Windows probes with a count of 0 to fetch just the header. When a
        // descriptor is wanted, report one flat region spanning the disc at our 2x rate.
        if(type == 0x00 && max_desc >= 1) {
            uint32_t last_lba = scsi_medium_blocks(scsi) - 1;
            scsi_store_be32(response + 8, 0); // start LBA
            scsi_store_be32(response + 12, PERFORMANCE_START); // start performance
            scsi_store_be32(response + 16, last_lba); // end LBA
            scsi_store_be32(response + 20, PERFORMANCE_END); // end performance
            response_len = 24;
        }
        scsi_store_be32(response, response_len - 4); // performance data length
        return scsi_tx_response(scsi, data, len, cap, response, response_len);
    }; break;
    case SCSI_MECHANISM_STATUS: {
        FURI_LOG_D(TAG, "SCSI_MECHANISM_STATUS");
        if(scsi->cmd_len < 12 || scsi->fn.device_type != MassStorageDeviceTypeOptical) {
            return false;
        }
        // Single-slot, non-changer mechanism, idle and fault-free: every field is zero except
        // the slot count. No slot tables follow (bytes 6..7 = 0).
        uint8_t response[8] = {0};
        response[5] = 0x01; // number of slots available
        return scsi_tx_response(scsi, data, len, cap, response, sizeof(response));
    }; break;
    default: {
        FURI_LOG_W(TAG, "unexpected scsi tx data cmd=%02X", scsi->cmd[0]);
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_COMMAND_OPERATION_CODE;
        return false;
    }; break;
    }
}

static bool scsi_finish_format(SCSISession* scsi) {
    const uint8_t* parameters = scsi->format.parameters;
    if(scsi->format.total != sizeof(scsi->format.parameters) || parameters[2] != 0 ||
       parameters[3] != 8 || (parameters[1] & 0x58)) {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
        return false;
    }

    uint32_t max_blocks = scsi->fn.num_blocks(scsi->fn.ctx);
    uint32_t blocks = scsi_read_be32(parameters + 4);
    uint8_t format_type = parameters[8] >> 2;
    uint8_t format_sub_type = parameters[8] & 0x03;
    uint32_t type_parameter = scsi_read_be24(parameters + 9);
    uint32_t packet_size = CD_RW_PACKET_SIZE;

    if(format_sub_type || blocks > max_blocks) {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
        return false;
    }

    if(format_type == 0x00) {
        if(type_parameter != scsi->fn.block_size) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
            return false;
        }
    } else if(format_type == 0x10) {
        if(!type_parameter) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
            return false;
        }
        packet_size = type_parameter;
    } else {
        scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
        scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
        return false;
    }

    if(!blocks) blocks = max_blocks;
    if(format_type == 0x10 && blocks % packet_size) {
        uint64_t rounded = (uint64_t)blocks + packet_size - blocks % packet_size;
        if(rounded > max_blocks) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_PARAMETER_LIST;
            return false;
        }
        blocks = rounded;
    }

    // Try-out validates the requested format without modifying the medium.
    if(parameters[1] & 0x04) return true;

    scsi->optical_formatted = true;
    scsi->formatted_blocks = blocks;
    scsi->optical_packet_size = packet_size;
    scsi->next_writable_lba = 0;
    scsi->reserved_blocks = 0;
    scsi->optical_open = false;
    scsi->optical_finalized = false;
    return scsi->fn.sync(scsi->fn.ctx);
}

static bool scsi_audio_play_range(SCSISession* scsi, uint32_t start_lba, uint32_t end_lba) {
    uint32_t blocks = scsi_medium_blocks(scsi);
    if(start_lba >= end_lba || end_lba > blocks) {
        scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OOB, 0);
        return false;
    }
    return scsi_audio_control(scsi, SCSIAudioControlPlay, start_lba, end_lba);
}

static bool scsi_audio_play_track_index(SCSISession* scsi, const uint8_t* cmd, uint8_t len) {
    if(len < 10) return false;
    uint8_t start_track = cmd[4];
    uint8_t start_index = cmd[5];
    uint8_t end_track = cmd[7];
    SCSIAudioTrackInfo start_info;
    if(!scsi_audio_track_info(scsi, start_track, &start_info) || start_index > 1) {
        scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
        return false;
    }
    uint32_t start_lba = start_index == 0 && start_info.has_index0 ? start_info.index0_lba :
                                                                     start_info.start_lba;
    uint32_t end_lba = scsi_medium_blocks(scsi);
    if(end_track != 0 && end_track != 0xAA) {
        SCSIAudioTrackInfo end_info;
        if(end_track < start_track || !scsi_audio_track_info(scsi, end_track, &end_info)) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
            return false;
        }
        end_lba = end_info.end_lba;
    }
    return scsi_audio_play_range(scsi, start_lba, end_lba);
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
    case SCSI_SEND_CUE_SHEET:
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
    case SCSI_READ_CD:
    case SCSI_READ_CD_MSF:
    case SCSI_READ_SUB_CHANNEL:
    case SCSI_READ_BUFFER_CAPACITY:
    case SCSI_GET_PERFORMANCE:
    case SCSI_MECHANISM_STATUS:
        return scsi->tx_done;

    case SCSI_TEST_UNIT_READY: {
        FURI_LOG_D(TAG, "SCSI_TEST_UNIT_READY");
        if(scsi->blank.active) {
            scsi_set_sense(
                scsi,
                SCSI_SK_NOT_READY,
                SCSI_ASC_LOGICAL_UNIT_NOT_READY,
                SCSI_ASCQ_OPERATION_IN_PROGRESS);
            return false;
        }
        if(scsi->blank.failed) return false;
        return true;
    }; break;
    case SCSI_REZERO_UNIT: {
        // Obsolete "seek to LBA 0". There is no physical mechanism to move, so acknowledge it
        // as a no-op; some hosts still issue it and expect GOOD status.
        FURI_LOG_D(TAG, "SCSI_REZERO_UNIT");
        if(scsi->fn.audio_cd) {
            return scsi_audio_control(scsi, SCSIAudioControlSeek, 0, scsi_medium_blocks(scsi));
        }
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
        FURI_LOG_D(
            TAG,
            "SCSI_START_STOP_UNIT immediate=%d eject=%d start=%d",
            !!(cmd[1] & 1),
            eject,
            start);
        if(scsi->blank.active && scsi_is_blank_cancel_command(cmd, len)) {
            FURI_LOG_D(TAG, "SCSI_BLANK canceled");
            if(!scsi->fn.sync(scsi->fn.ctx)) {
                scsi_finish_blank(scsi, false);
                return false;
            }
            scsi_blank_cancel(scsi);
        }
        if(eject && !start) {
            if(!scsi->fn.sync(scsi->fn.ctx)) return false;
            if(scsi->fn.audio_cd && !scsi_audio_control(scsi, SCSIAudioControlStop, 0, 0)) {
                return false;
            }
            scsi->eject_pending = true;
        } else if(scsi->fn.audio_cd && !start) {
            return scsi_audio_control(scsi, SCSIAudioControlStop, 0, 0);
        }
        return true;
    }; break;
    case SCSI_PLAY_AUDIO_10: {
        if(len < 10 || !scsi->fn.audio_cd) return false;
        uint32_t start_lba = scsi_read_be32(cmd + 2);
        uint32_t count = cmd[7] << 8 | cmd[8];
        FURI_LOG_D(TAG, "SCSI_PLAY_AUDIO_10 %08lX %04lX", start_lba, count);
        if(count == 0) return true;
        uint64_t end_lba = (uint64_t)start_lba + count;
        if(end_lba > UINT32_MAX) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OOB, 0);
            return false;
        }
        return scsi_audio_play_range(scsi, start_lba, end_lba);
    }; break;
    case SCSI_PLAY_AUDIO_12: {
        if(len < 12 || !scsi->fn.audio_cd) return false;
        uint32_t start_lba = scsi_read_be32(cmd + 2);
        uint32_t count = scsi_read_be32(cmd + 6);
        FURI_LOG_D(TAG, "SCSI_PLAY_AUDIO_12 %08lX %08lX", start_lba, count);
        if(count == 0) return true;
        uint64_t end_lba = (uint64_t)start_lba + count;
        if(end_lba > UINT32_MAX) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OOB, 0);
            return false;
        }
        return scsi_audio_play_range(scsi, start_lba, end_lba);
    }; break;
    case SCSI_PLAY_AUDIO_MSF: {
        if(len < 10 || !scsi->fn.audio_cd) return false;
        uint32_t start_lba;
        uint32_t end_lba;
        bool current_position = cmd[3] == 0xFF && cmd[4] == 0xFF && cmd[5] == 0xFF;
        if(current_position) {
            SCSIAudioStatus status;
            if(!scsi->fn.audio_get_status || !scsi->fn.audio_get_status(scsi->fn.ctx, &status)) {
                return false;
            }
            start_lba = status.lba;
        } else if(!scsi_cdrom_address_to_lba(cmd + 3, &start_lba)) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
            return false;
        }
        if(!scsi_cdrom_address_to_lba(cmd + 6, &end_lba)) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
            return false;
        }
        FURI_LOG_D(TAG, "SCSI_PLAY_AUDIO_MSF %08lX %08lX", start_lba, end_lba);
        return scsi_audio_play_range(scsi, start_lba, end_lba);
    }; break;
    case SCSI_PLAY_AUDIO_TRACK_INDEX:
        FURI_LOG_D(TAG, "SCSI_PLAY_AUDIO_TRACK_INDEX");
        return scsi_audio_play_track_index(scsi, cmd, len);
    case SCSI_PAUSE_RESUME: {
        if(len < 10 || !scsi->fn.audio_cd) return false;
        bool resume = cmd[8] & 0x01;
        FURI_LOG_D(TAG, "SCSI_PAUSE_RESUME resume=%u", resume);
        return scsi_audio_control(
            scsi, resume ? SCSIAudioControlResume : SCSIAudioControlPause, 0, 0);
    }; break;
    case SCSI_STOP_PLAY_SCAN:
        FURI_LOG_D(TAG, "SCSI_STOP_PLAY_SCAN");
        return len >= 10 && scsi_audio_control(scsi, SCSIAudioControlStop, 0, 0);
    case SCSI_SCAN: {
        if(len < 12 || !scsi->fn.audio_cd) return false;
        uint8_t address_type = cmd[9] >> 6;
        uint32_t start_lba;
        if(address_type == 0) {
            start_lba = scsi_read_be32(cmd + 2);
        } else if(address_type == 1) {
            if(!scsi_cdrom_address_to_lba(cmd + 3, &start_lba)) {
                scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
                return false;
            }
        } else if(address_type == 2) {
            SCSIAudioTrackInfo info;
            if(!scsi_audio_track_info(scsi, cmd[5], &info)) {
                scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
                return false;
            }
            start_lba = info.start_lba;
        } else {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB, 0);
            return false;
        }
        if(start_lba >= scsi_medium_blocks(scsi)) {
            scsi_set_sense(scsi, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OOB, 0);
            return false;
        }
        bool reverse = cmd[1] & 0x10;
        FURI_LOG_D(TAG, "SCSI_SCAN lba=%08lX reverse=%u", start_lba, reverse);
        return scsi_audio_control(
            scsi,
            reverse ? SCSIAudioControlScanBackward : SCSIAudioControlScanForward,
            start_lba,
            scsi_medium_blocks(scsi));
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
        uint32_t num_blocks = scsi_medium_blocks(scsi);
        uint32_t remaining =
            scsi->next_writable_lba < num_blocks ? num_blocks - scsi->next_writable_lba : 0;
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
    case SCSI_BLANK: {
        scsi_start_blank(scsi, cmd[1] & 0x10);
        return true;
    }; break;
    case SCSI_FORMAT_UNIT: {
        if(scsi->fn.device_type != MassStorageDeviceTypeOptical || scsi->fn.read_only) {
            return false;
        }
        if(!scsi->rx_done) return false;
        FURI_LOG_D(TAG, "SCSI_FORMAT_UNIT done");
        return scsi_finish_format(scsi);
    }; break;
    case SCSI_SET_CD_SPEED: {
        FURI_LOG_D(TAG, "SCSI_SET_CD_SPEED");
        return scsi->fn.device_type == MassStorageDeviceTypeOptical;
    }; break;
    case SCSI_SEEK_10: {
        if(len < 10 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        uint32_t lba = scsi_read_be32(cmd + 2);
        FURI_LOG_D(TAG, "SCSI_SEEK_10 %08lX", lba);
        if(lba >= scsi_medium_blocks(scsi)) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_LBA_OOB;
            return false;
        }
        if(scsi->fn.audio_cd) {
            return scsi_audio_control(scsi, SCSIAudioControlSeek, lba, scsi_medium_blocks(scsi));
        }
        // Nothing physically moves; just validate the address and acknowledge.
        return true;
    }; break;
    case SCSI_SEND_DIAGNOSTIC: {
        if(len < 6 || scsi->fn.device_type != MassStorageDeviceTypeOptical) return false;
        FURI_LOG_D(TAG, "SCSI_SEND_DIAGNOSTIC");
        // Only the default self-test is supported, and it always passes. A non-zero parameter
        // list length would mean a data-out page we do not implement, so reject that form.
        uint16_t param_len = cmd[3] << 8 | cmd[4];
        if(param_len) {
            scsi->sk = SCSI_SK_ILLEGAL_REQUEST;
            scsi->asc = SCSI_ASC_INVALID_FIELD_IN_CDB;
            return false;
        }
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
