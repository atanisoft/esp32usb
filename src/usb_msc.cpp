// Copyright 2020 Mike Dunston (https://github.com/atanisoft)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define MSC_LOG_LEVEL_BOOT_SECTOR ESP_LOG_VERBOSE
#define MSC_LOG_LEVEL_ROOT_DIRECTORY ESP_LOG_VERBOSE
#define MSC_LOG_LEVEL_FAT_TABLE ESP_LOG_VERBOSE

//#define LOG_LOCAL_LEVEL 6

#include "usb.h"

#if CONFIG_TINYUSB_MSC_ENABLED

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <endian.h>
#include <vector>

static constexpr const char * const TAG = "USB:MSC";

enum PARTITION_TYPE_T : uint8_t
{
    PART_EMPTY = 0x00,
    PART_FAT_12 = 0x01,
    PART_FAT_16 = 0x04,
    PART_FAT_16B = 0x06,
    PART_FAT_32_LBA = 0x0C,
    PART_FAT_16B_LBA = 0x0E,
    PART_EXTENDED = 0x0F,
};

enum PARTITION_STATUS : uint8_t
{
    PART_STATUS_UNUSED = 0x00,
    PART_STATUS_ACTIVE = 0x80,
    PART_STATUS_BOOTABLE = 0x80
};

typedef struct TU_ATTR_PACKED
{
    PARTITION_STATUS  status;               //  1 status of the disk:
                                            //    0x00 = inactive
                                            //    0x01-0x7f = invalid
                                            //    0x80 = bootable
    uint8_t first_head;                     //  2 
    uint8_t first_sector;                   //  3 this field is split between sector and cylinder:
                                            //    bits 0-5 (0x3F) are for sector
                                            //    bits 6,7 are cylinder bits 8,9
    uint8_t first_cylinder;                 //  4
    PARTITION_TYPE_T partition_type;        //  6
    uint8_t last_head;                      //  7
    uint8_t last_sector;                    //  8 this field is split between sector and cylinder:
                                            //    bits 0-5 (0x3F) are for sector
                                            //    bits 6,7 are cylinder bits 8,9
    uint8_t last_cylinder;                  //  9
    uint32_t first_lba;                     // 12
    uint32_t sector_count;                  // 16
} partition_def_t;

static_assert(sizeof(partition_def_t) == 16,
              "partition_def_t should be 16 bytes");

typedef struct TU_ATTR_PACKED
{
    uint8_t bootstrap[218];                 // 218 -- unused today.
    uint16_t disk_timestamp;                // 220
    uint8_t original_drive_id;              // 221
    uint8_t disk_seconds;                   // 222
    uint8_t disk_minutes;                   // 223
    uint8_t disk_hours;                     // 224
    uint8_t boostrap2[216];                 // 440 NOTE: this may spill over into next two fields.
    uint32_t disk_signature;                // 444
    uint16_t copy_protected;                // 446
    partition_def_t partitions[4];          // 510
    uint8_t boot_signature[2];              // 512 -- boot sector signature
} master_boot_record_t;

static_assert(sizeof(master_boot_record_t) == 512,
              "master_boot_record_t should be 512 bytes");

typedef struct TU_ATTR_PACKED
{
    uint8_t jump_instruction[3];            //   3 -- boot sector
    uint8_t oem_info[8];                    //  11
    uint16_t sector_size;                   //  13 -- bios param block
    uint8_t sectors_per_cluster;            //  14
    uint16_t reserved_sectors;              //  16
    uint8_t fat_copies;                     //  17
    uint16_t root_directory_entries;        //  19
    uint16_t total_sectors_16;              //  21
    uint8_t media_descriptor;               //  22
    uint16_t sectors_per_fat;               //  24
    uint16_t sectors_per_track;             //  26
    uint16_t heads;                         //  28
    uint32_t hidden_sectors;                //  32
    uint32_t total_sectors_32;              //  36
    uint8_t physical_drive_num;             //  37 -- extended boot record
    uint8_t reserved;                       //  38
    uint8_t extended_boot_sig;              //  39
    uint32_t volume_serial_number;          //  43
    char volume_label[11];                  //  54
    uint8_t filesystem_identifier[8];       //  62
    uint8_t reserved2[510 - 62];            // 510
    uint8_t boot_signature[2];              // 512 -- boot sector signature
} boot_sector_t;

static_assert(sizeof(boot_sector_t) == 512,
              "boot_sector_t should be 512 bytes");

typedef struct TU_ATTR_PACKED
{
    char name[8];
    char ext[3];
    uint8_t attrs;
    uint8_t reserved;
    uint8_t create_time_fine;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t high_start_cluster;
    uint16_t update_time;
    uint16_t update_date;
    uint16_t start_cluster;
    uint32_t size;
} fat_direntry_t;

static_assert(sizeof(fat_direntry_t) == 32,
              "fat_direntry_t should be 32 bytes");

typedef enum
{
    DIRENT_READ_ONLY = 0x01,
    DIRENT_HIDDEN = 0x02,
    DIRENT_SYSTEM = 0x04,
    DIRENT_VOLUME_LABEL = 0x08,
    DIRENT_SUB_DIRECTORY = 0x10,
    DIRENT_ARCHIVE = 0x20,
    DIRENT_DEVICE = 0x40,
    DIRENT_RESERVED = 0x80
} FAT_DIRENTRY_ATTRS;

typedef struct
{
    char name[8];
    char ext[3];
    const char *content;
    uint8_t flags;
    uint32_t size;
    uint32_t start_sector;
    uint32_t end_sector;
    uint16_t start_cluster;
    uint16_t end_cluster;
    const esp_partition_t *partition;
    std::string printable_name;
} fat_file_entry_t;

static const uint16_t DISK_SECTOR_SIZE = 512;
static const uint16_t FAT_BLOCK_COUNT = 8192;
static const uint8_t RESERVED_SECTOR_COUNT = 1;
static const uint8_t FAT_ROOT_DIR_SECTORS = 4;

static const uint16_t DIRENTRIES_PER_SECTOR =
    (DISK_SECTOR_SIZE / sizeof(fat_direntry_t));
static const uint16_t SECTORS_PER_FAT_BLOCK = 
    ((FAT_BLOCK_COUNT * 2) + (DISK_SECTOR_SIZE - 1)) / DISK_SECTOR_SIZE;
static const uint16_t ROOT_DIRECTORY_ENTRIES =
    FAT_ROOT_DIR_SECTORS * DIRENTRIES_PER_SECTOR;

static const uint16_t FAT_COPY_0_FIRST_SECTOR = RESERVED_SECTOR_COUNT;
static const uint16_t FAT_COPY_1_FIRST_SECTOR =
    FAT_COPY_0_FIRST_SECTOR + SECTORS_PER_FAT_BLOCK;
static const uint16_t ROOT_DIR_FIRST_SECTOR =
    FAT_COPY_1_FIRST_SECTOR + SECTORS_PER_FAT_BLOCK;
static const uint16_t FILE_CONTENT_FIRST_SECTOR =
    ROOT_DIR_FIRST_SECTOR + FAT_ROOT_DIR_SECTORS;

static boot_sector_t s_boot_sector =
{
    .jump_instruction = {0xEB, 0x3C, 0x90},
    .oem_info = {'M','S','D','O','S','5','.','0'},
    .sector_size = DISK_SECTOR_SIZE,
    .sectors_per_cluster = 1,
    .reserved_sectors = RESERVED_SECTOR_COUNT,
    .fat_copies = 2,
    .root_directory_entries = ROOT_DIRECTORY_ENTRIES,
    .total_sectors_16 = FAT_BLOCK_COUNT,
    .media_descriptor = 0xF8,
    .sectors_per_fat = SECTORS_PER_FAT_BLOCK,
    .sectors_per_track = 1,
    .heads = 1,
    .hidden_sectors = 0,
    .total_sectors_32 = 0,
    .physical_drive_num = 0x80,
    .reserved = 0,
    .extended_boot_sig = 0x29,
    .volume_serial_number = 0,
    .volume_label = {'e','s','p','3','2','s','2'},
    .filesystem_identifier = {'F','A','T','1','6',' ',' ',' '},
    .reserved2 = {0},
    .boot_signature = {0x55, 0xaa}
};
static std::vector<fat_file_entry_t> s_root_directory;

static const char * const s_vendor_id = CONFIG_TINYUSB_MSC_VENDOR_ID;
static const char * const s_product_id = CONFIG_TINYUSB_MSC_PRODUCT_ID;
static const char * const s_product_rev = CONFIG_TINYUSB_MSC_PRODUCT_REVISION;

static void space_padded_memcpy(char *dst, const char *src, int len)
{
    for (int i = 0; i < len; ++i)
    {
        *dst++ = *src ? *src++ : ' ';
    }
}

void configure_virtual_disk(std::string label, uint32_t serial_number)
{
    space_padded_memcpy(s_boot_sector.volume_label, label.c_str(), 11);
    s_boot_sector.volume_serial_number = htole32(serial_number);
    ESP_LOGI(TAG,
             "USB Virtual disk %-11.11s\n"
             "%d total sectors (%d bytes)\n"
             "%d reserved sector(s)\n"
             "%d sectors per FAT block (%d bytes)\n"
             "fat0 sector start: %d\n"
             "fat1 sector start: %d\n"
             "root directory sector start: %d (%d entries, %d per sector)\n"
             "first file sector start: %d\n"
           , s_boot_sector.volume_label
           , s_boot_sector.total_sectors_16
           , s_boot_sector.total_sectors_16 * s_boot_sector.sector_size
           , s_boot_sector.reserved_sectors
           , s_boot_sector.sectors_per_fat
           , s_boot_sector.sectors_per_fat * s_boot_sector.sector_size
           , FAT_COPY_0_FIRST_SECTOR
           , FAT_COPY_1_FIRST_SECTOR
           , ROOT_DIR_FIRST_SECTOR
           , ROOT_DIRECTORY_ENTRIES
           , FILE_CONTENT_FIRST_SECTOR
           , DIRENTRIES_PER_SECTOR
    );
    // convert fields to little endian
    s_boot_sector.sector_size = htole16(s_boot_sector.sector_size);
    s_boot_sector.reserved_sectors = htole16(s_boot_sector.reserved_sectors);
    s_boot_sector.root_directory_entries = htole16(s_boot_sector.root_directory_entries);
    s_boot_sector.total_sectors_16 = htole16(s_boot_sector.total_sectors_16);
    s_boot_sector.sectors_per_fat = htole16(s_boot_sector.sectors_per_fat);
    s_boot_sector.sectors_per_track = htole16(s_boot_sector.sectors_per_track);
    s_boot_sector.heads = htole16(s_boot_sector.heads);
    s_boot_sector.hidden_sectors = htole32(s_boot_sector.hidden_sectors);
    s_boot_sector.heads = htole32(s_boot_sector.heads);
}

void register_virtual_file(const std::string name, const char *content,
                           uint32_t size, bool read_only,
                           const esp_partition_t *partition)
{
    fat_file_entry_t file;
    // zero out the file descriptor block
    bzero(&file, sizeof(fat_file_entry_t));

    // default base name and extension to spaces
    memset(file.name, ' ', TU_ARRAY_SIZE(file.name));
    memset(file.ext, ' ', TU_ARRAY_SIZE(file.ext));

    // break the provided filename into base name and extension
    size_t pos = name.find_first_of('.');
    std::string base_name = name;
    std::string extension = "";
    if (pos == std::string::npos)
    {
        // truncate the base name to a max of eleven characters, including the
        // file extension (if present)
        if (base_name.length() > 11)
        {
            base_name.resize(11);
        }
        // copy the filename as-is into the base name and extension fields with
        // space padding as needed. NOTE: this will overflow the name field.
        strncpy(file.name, base_name.c_str(), base_name.length());
        file.printable_name.assign(std::move(base_name));
    }
    else
    {
        base_name = name.substr(0, pos);
        extension = name.substr(pos + 1, 3);
        // truncate the base name to a max of eight characters
        if (base_name.length() > 8)
        {
            base_name.resize(8);
        }
        strncpy(file.name, base_name.c_str(), base_name.length());
        strncpy(file.ext, extension.c_str(), extension.length());
        file.printable_name.assign(std::move(base_name)).append(".").append(extension);
    }
    file.content = content;
    file.partition = partition;
    file.size = size;
    file.flags = FAT_DIRENTRY_ATTRS::DIRENT_ARCHIVE;
    if (read_only)
    {
        file.flags |= FAT_DIRENTRY_ATTRS::DIRENT_READ_ONLY;
    }

    if (s_root_directory.empty())
    {
        file.start_sector = FILE_CONTENT_FIRST_SECTOR;
        file.start_cluster = 2;
    }
    else
    {
        file.start_sector = s_root_directory.back().end_sector + 1;
        file.start_cluster = s_root_directory.back().end_cluster + 1;
    }
    file.end_sector = file.start_sector;
    file.end_sector += (size / s_boot_sector.sector_size);
    file.end_cluster = file.start_cluster;
    file.end_cluster += (size / s_boot_sector.sector_size);
    s_root_directory.push_back(file);
    ESP_LOGI(TAG,
             "File(%s) sectors: %d - %d, clusters: %d - %d, %d bytes",
             file.printable_name.c_str(), file.start_sector, file.end_sector,
             file.start_cluster, file.end_cluster, size);
}

void add_readonly_file_to_virtual_disk(const std::string filename,
                                       const char *content, uint32_t size)
{
    register_virtual_file(filename, content, size, true, nullptr);
}

void add_partition_to_virtual_disk(const std::string partition_name, const std::string filename, bool writable)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_ANY,
                                 partition_name.c_str());
    if (part == nullptr)
    {
        // try and find it as a data partition
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_ANY,
                                        partition_name.c_str());
    }
    if (part != nullptr)
    {
        register_virtual_file(filename, nullptr, part->size, writable, part);
    }
}

void add_firmware_to_virtual_disk(const std::string current_name, const std::string previous_name)
{
    const esp_partition_t *current_part = esp_ota_get_running_partition();
    const esp_partition_t *next_part = esp_ota_get_next_update_partition(nullptr);
    register_virtual_file(current_name, nullptr, current_part->size, true, current_part);
    if (next_part != nullptr && next_part != current_part)
    {
        register_virtual_file(previous_name, nullptr, next_part->size, false, next_part);
    }
}

// =============================================================================
// TinyUSB CALLBACKS
// =============================================================================

extern "C"
{

// Invoked for SCSI_CMD_INQUIRY command.
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    memcpy(vendor_id, s_vendor_id, strlen(s_vendor_id) > 8 ? 8 : strlen(s_vendor_id));
    memcpy(product_id, s_product_id, strlen(s_product_id) > 16 ? 16 : strlen(s_product_id));
    memcpy(product_rev, s_product_rev, strlen(s_product_rev) > 4 ? 4 : strlen(s_product_rev));
}

// Invoked for Test Unit Ready command.
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    return true;
}

// Invoked for SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY
// to determine the disk size.
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size)
{
    *block_count = s_boot_sector.total_sectors_16;
    *block_size  = s_boot_sector.sector_size;
}

// Callback for READ10 command.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    bzero(buffer, bufsize);
    if (lba == 0)
    {
        // Requested boot block
        memcpy(buffer, &s_boot_sector, bufsize);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize, MSC_LOG_LEVEL_BOOT_SECTOR);
    }
    else if (lba < ROOT_DIR_FIRST_SECTOR)
    {
        bool table_modified = false;
        uint32_t fat_table = (lba - FAT_COPY_0_FIRST_SECTOR);
        if (fat_table > s_boot_sector.sectors_per_fat)
        {
            fat_table -= s_boot_sector.sectors_per_fat;
        }
        uint32_t fat_table_start_sector = fat_table * 256;
        uint32_t fat_table_end_sector = (fat_table + 1) * 256;
        ESP_LOGV(TAG, "FAT Table: %d (sector: %d -> %d)", fat_table,
                 fat_table_start_sector, fat_table_end_sector);
        uint16_t *buf_16 = (uint16_t *)buffer;
        if (fat_table == 0)
        {
            table_modified = true;
            // cluster zero is reserved for FAT ID and media descriptor.
            buf_16[0] = htole16(0xFF | s_boot_sector.media_descriptor);
            // cluster one is reserved.
            buf_16[1] = htole16(0xFFFF);
        }
        for(auto &file : s_root_directory)
        {
            uint32_t start_sector = file.start_sector;
            uint32_t end_sector = file.end_sector;
            if (fat_table_start_sector >= start_sector &&
                fat_table_start_sector <= end_sector)
            {
                table_modified = true;
                for(size_t fat_cluster_index = 0; fat_cluster_index < 256;
                    fat_cluster_index++)
                {
                    // convert fat_cluster_index into the on-disk sector
                    uint32_t sector = (fat_table * 256) + fat_cluster_index;
                    if (sector > start_sector && sector < end_sector)
                    {
                        buf_16[fat_cluster_index] = htole16(sector + 1);
                    }
                    else if (sector == end_sector)
                    {
                        buf_16[fat_cluster_index] = htole16(0xFFFF);
                    }
                }
            }
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize, MSC_LOG_LEVEL_FAT_TABLE);
    }
    else if (lba < FILE_CONTENT_FIRST_SECTOR)
    {
        ESP_LOGV(TAG, "ROOT DIRECTORY\n");
        // Requested sector of the root directory
        uint32_t sector_idx = (lba - ROOT_DIR_FIRST_SECTOR);
        if (sector_idx == 0)
        {
            fat_direntry_t *d = static_cast<fat_direntry_t *>(buffer);
            // NOTE this will overrun d->name and spill over into d->ext
            memcpy(d->name, s_boot_sector.volume_label, 11);
            d->attrs = FAT_DIRENTRY_ATTRS::DIRENT_ARCHIVE | FAT_DIRENTRY_ATTRS::DIRENT_VOLUME_LABEL;
            // move beyond the first entry
            d++;
            for(auto &file : s_root_directory)
            {
                // note this will clear the file extension.
                space_padded_memcpy(d->name, file.name, 11);
                space_padded_memcpy(d->ext, file.ext, 3);
                d->attrs = file.flags;
                d->size = file.size;
                d->start_cluster = file.start_cluster;
                d->create_date = 0x4d99;
                d->update_date = 0x4d99;
                // move to next directory entry
                d++;
            }
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize, MSC_LOG_LEVEL_ROOT_DIRECTORY);
    }
    else
    {
        // scan the root directory entries for a file that is in the requested sector.
        for(auto &file : s_root_directory)
        {
            if (lba >= file.start_sector && lba <= file.end_sector)
            {
                // translate the LBA into the on-disk sector index
                uint32_t sector_idx = lba - file.start_sector;

                size_t temp_size = bufsize;
                size_t sector_offset =
                    (sector_idx * s_boot_sector.sector_size) + offset;
                uint32_t file_size = file.size;
                // bounds check to ensure the read does not go beyond the
                // recorded file size.
                if (bufsize > (file_size - sector_offset))
                {
                    temp_size = file_size - sector_offset;
                }
                ESP_LOGV(TAG, "File(%s) READ %d bytes from lba:%d (offs:%d)",
                         file.printable_name.c_str(), temp_size, lba, offset);

                if (file.partition != nullptr)
                {
                    if (ESP_ERROR_CHECK_WITHOUT_ABORT(
                            esp_partition_read(file.partition, sector_offset,
                                                buffer, temp_size)) != ESP_OK)
                    {
                        return -1;
                    }
                }
                else
                {
                    memcpy(buffer, file.content + sector_offset, temp_size);
                }
            }
        }
    }

    return bufsize;
}

// Callback for WRITE10 command.
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t* buffer, uint32_t bufsize)
{
    if (lba == 0)
    {
        ESP_LOGV(TAG, "Write to BOOT sector\n");
    }
    else if (lba < ROOT_DIR_FIRST_SECTOR)
    {
        ESP_LOGV(TAG, "Write to FAT cluster chain\n");
    }
    else if (lba < FILE_CONTENT_FIRST_SECTOR)
    {
        ESP_LOGV(TAG, "write to root directory\n");
    }
    else
    {
        // scan the root directory entries for a file that is in the requested
        // sector. Check if it is flagged as read-only and reject the write
        // attempt.
        for(auto &file : s_root_directory)
        {
            if (lba >= file.start_sector && lba <= file.end_sector)
            {
                if (file.flags & FAT_DIRENTRY_ATTRS::DIRENT_READ_ONLY)
                {
                    ESP_LOGV(TAG, "Attempt to write to read only file.");
                    return -1;
                }
                else if (file.content != nullptr)
                {
                    // translate the LBA into the on-disk sector index
                    uint32_t sector_idx = lba - file.start_sector;
                    size_t temp_size = bufsize;
                    size_t sector_offset =
                        (sector_idx * s_boot_sector.sector_size) + offset;
                    uint32_t file_size = file.size;
                    // bounds check to ensure the write does not go beyond the
                    // recorded file size.
                    if (bufsize > (file_size - sector_offset))
                    {
                        temp_size = file_size - sector_offset;
                    }
                    uint8_t *buf = ((uint8_t *)file.content) + sector_offset;
                    memcpy(buf, buffer, temp_size);
                }
            }
        }
    }
    return bufsize;
}

// Callback for SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 have their own callbacks
int32_t tud_msc_scsi_cb (uint8_t lun, uint8_t const scsi_cmd[16], void* buffer,
                         uint16_t bufsize)
{
    void const *response = NULL;
    uint16_t resplen = 0;

    // most scsi handled is input
    bool in_xfer = true;

    switch (scsi_cmd[0])
    {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        // Host is about to read/write etc ... better not to disconnect disk
        resplen = 0;
        break;

    default:
        // Set Sense = Invalid Command Operation
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

        // negative means error -> tinyusb could stall and/or response with
        // failed status
        resplen = -1;
        break;
    }

    // return resplen must not larger than bufsize
    if (resplen > bufsize)
        resplen = bufsize;

    if (response && (resplen > 0))
    {
        if (in_xfer)
        {
            memcpy(buffer, response, resplen);
        }
        else
        {
            // SCSI output
        }
    }

    return resplen;
}

} // extern "C"

#endif // CONFIG_TINYUSB_MSC_ENABLED