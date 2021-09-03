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

#include "usb.h"

#if CONFIG_ESPUSB_MSC

// these can be used to fine tune the debug log levels for hex dump of sector
// data within the read callback.
#define MSC_LOG_LEVEL_BOOT_SECTOR ESP_LOG_VERBOSE
#define MSC_LOG_LEVEL_ROOT_DIRECTORY ESP_LOG_VERBOSE
#define MSC_LOG_LEVEL_FAT_TABLE ESP_LOG_VERBOSE
// in order for debug data to be printed this needs to be defined prior to
// inclusion of esp_log.h. The value below is one higher than ESP_LOG_VERBOSE.
//#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include <endian.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <vector>
#include "psram_allocator.h"

static constexpr const char * const TAG = "USB:MSC";

typedef enum : uint8_t
{
    PART_EMPTY = 0x00,
    PART_FAT_12 = 0x01,
    PART_FAT_16 = 0x04,
    PART_FAT_16B = 0x06,
    PART_FAT_32_LBA = 0x0C,
    PART_FAT_16B_LBA = 0x0E,
    PART_EXTENDED = 0x0F,
} partition_type_t;

typedef enum : uint8_t
{
    PART_STATUS_UNUSED = 0x00,
    PART_STATUS_ACTIVE = 0x80,
    PART_STATUS_BOOTABLE = 0x80
} partition_status_t;

typedef struct TU_ATTR_PACKED               //  start
{                                           // offset notes
    partition_status_t  status;             //   0x00 status of the disk:
                                            //        0x00 = inactive
                                            //        0x01-0x7f = invalid
                                            //        0x80 = bootable
    uint8_t first_head;                     //   0x01
    uint8_t first_sector;                   //   0x02 this field is split between sector and cylinder:
                                            //        bits 0-5 (0x3F) are for sector
                                            //        bits 6,7 are cylinder bits 8,9
    uint8_t first_cylinder;                 //   0x03
    partition_type_t partition_type;        //   0x04
    uint8_t last_head;                      //   0x05
    uint8_t last_sector;                    //   0x06 this field is split between sector and cylinder:
                                            //        bits 0-5 (0x3F) are for sector
                                            //        bits 6,7 are cylinder bits 8,9
    uint8_t last_cylinder;                  //   0x07
    uint32_t first_lba;                     //   0x08
    uint32_t sector_count;                  //   0x0C
} partition_def_t;

static_assert(sizeof(partition_def_t) == 16,
              "partition_def_t should be 16 bytes");

typedef struct TU_ATTR_PACKED               //  start
{                                           // offset notes
    uint8_t bootstrap[218];                 //  0x000 bootstrap code.
    uint16_t disk_timestamp;                //  0x0DA
    uint8_t original_drive_id;              //  0x0DC
    uint8_t disk_seconds;                   //  0x0DD
    uint8_t disk_minutes;                   //  0x0DE
    uint8_t disk_hours;                     //  0x0DF
    uint8_t boostrap2[216];                 //  0x0E0 bootstrap code part 2
    uint32_t disk_signature;                //  0x1B8
    uint16_t copy_protected;                //  0x1BC
    partition_def_t partitions[4];          //  0x1BE four partition tables
    uint8_t signature[2];                   //  0x1FE signature, 0x55, 0xAA
} master_boot_record_t;

static_assert(sizeof(master_boot_record_t) == 512,
              "master_boot_record_t should be 512 bytes");

typedef struct TU_ATTR_PACKED               //  start
{                                           // offset notes
    uint8_t jump_instruction[3];            //  0x000
    uint8_t oem_info[8];                    //  0x003
    uint16_t sector_size;                   //  0x00B bios param block
    uint8_t sectors_per_cluster;            //  0x00D
    uint16_t reserved_sectors;              //  0x00E
    uint8_t fat_copies;                     //  0x010
    uint16_t root_directory_entries;        //  0x011
    uint16_t sector_count_16;               //  0x013
    uint8_t media_descriptor;               //  0x015
    uint16_t fat_sectors;                   //  0x016
    uint16_t sectors_per_track;             //  0x018 DOS 3.31 BPB
    uint16_t heads;                         //  0x01A
    uint32_t hidden_sectors;                //  0x01C
    uint32_t sector_count_32;               //  0x020
    uint8_t drive_num;                      //  0x024 extended boot param block (FAT 12/16)
    uint8_t reserved;                       //  0x025
    uint8_t boot_sig;                       //  0x026
    uint32_t volume_serial_number;          //  0x027
    char volume_label[11];                  //  0x02B only available if boot_sig = 0x29
    uint8_t fs_identifier[8];               //  0x036 only available if boot_sig = 0x29
    uint8_t boot_code[0x1FE - 0x03E];       //  0x03E
    uint8_t signature[2];                   //  0x1FE signature, 0x55, 0xAA
} bios_boot_sector_t;

static_assert(sizeof(bios_boot_sector_t) == 512,
              "bios_boot_sector_t should be 512 bytes");

typedef enum : uint8_t
{
    DIRENT_READ_ONLY = 0x01,
    DIRENT_HIDDEN = 0x02,
    DIRENT_SYSTEM = 0x04,
    DIRENT_VOLUME_LABEL = 0x08,
    DIRENT_SUB_DIRECTORY = 0x10,
    DIRENT_ARCHIVE = 0x20,
    DIRENT_DEVICE = 0x40,
    DIRENT_RESERVED = 0x80
} dirent_attr_t;

typedef struct TU_ATTR_PACKED               //  start
{                                           // offset notes
    char name[8];                           //   0x00 space padded
    char ext[3];                            //   0x08 space padded
    uint8_t attributes;                     //   0x0B bitmask of dirent_attr_t
    uint8_t reserved;                       //   0x0C
    uint8_t create_time_fine;               //   0x0D
    uint16_t create_time;                   //   0x0E bits 15-11 are hours, 10-5 minutes, 4-0 seconds
    uint16_t create_date;                   //   0x10 bits 15-9 are year (0=1980), 8-5 month, 4-0 day
    uint16_t last_access_date;              //   0x12 same format as create_date
    uint16_t high_start_cluster;            //   0x14 FAT-32 only, high bytes for cluster start
    uint16_t update_time;                   //   0x16 same format as create_time
    uint16_t update_date;                   //   0x18 same format as create_date
    uint16_t start_cluster;                 //   0x1A starting cluster (FAT-16), low order bytes for FAT-32.
    uint32_t size;                          //   0x1C
} fat_direntry_t;

static_assert(sizeof(fat_direntry_t) == 32,
              "fat_direntry_t should be 32 bytes");

typedef struct TU_ATTR_PACKED               //  start
{                                           // offset notes
    uint8_t sequence;                       //   0x00 bit 6 indicates last in sequence, bits 0-5 are index.
    uint16_t name[5];                       //   0x01 first five UTF-16 characters of name
    uint8_t attributes;                     //   0x0B always 0x0F
    uint8_t type;                           //   0x0C always 0x00
    uint8_t checksum;                       //   0x0D
    uint16_t name2[6];                      //   0x0E next six UTF-16 characters of name
    uint16_t start_cluster;                 //   0x1A always 0x0000
    uint16_t name3[2];                      //   0x1C last two UTF-16 charactes of name
} fat_long_filename_t;

static_assert(sizeof(fat_long_filename_t) == sizeof(fat_direntry_t),
              "fat_long_filename_t should be same size as fat_direntry_t");

typedef struct
{
    char name[8];
    char ext[3];
    const char *content;
    uint8_t attributes;
    uint32_t size;
    uint32_t start_sector;
    uint32_t end_sector;
    uint16_t start_cluster;
    uint16_t end_cluster;
    const esp_partition_t *partition;
    std::string printable_name;
    uint8_t root_dir_sector;
#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
    std::vector<fat_long_filename_t,
                PSRAMAllocator<fat_long_filename_t>> lfn_parts;
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES
} fat_file_entry_t;

static_assert((CONFIG_ESPUSB_MSC_VDISK_FILE_COUNT & 15) == 0,
              "Number of files on the virtual disk must be a multiple of 16");

static constexpr uint16_t DIRENTRIES_PER_SECTOR =
    (CONFIG_ESPUSB_MSC_VDISK_SECTOR_SIZE / sizeof(fat_direntry_t));
static constexpr uint16_t SECTORS_PER_FAT_TABLE = 
    ((CONFIG_ESPUSB_MSC_VDISK_SECTOR_COUNT * 2) +
     (CONFIG_ESPUSB_MSC_VDISK_SECTOR_SIZE - 1))
    / CONFIG_ESPUSB_MSC_VDISK_SECTOR_SIZE;

static constexpr uint16_t FAT_COPY_0_FIRST_SECTOR =
    CONFIG_ESPUSB_MSC_VDISK_RESERVED_SECTOR_COUNT;
static constexpr uint16_t FAT_COPY_1_FIRST_SECTOR =
    FAT_COPY_0_FIRST_SECTOR + SECTORS_PER_FAT_TABLE;
static constexpr uint16_t ROOT_DIR_SECTOR_COUNT = 
    (CONFIG_ESPUSB_MSC_VDISK_FILE_COUNT / DIRENTRIES_PER_SECTOR);
static constexpr uint16_t ROOT_DIR_FIRST_SECTOR =
    FAT_COPY_1_FIRST_SECTOR + SECTORS_PER_FAT_TABLE;
static constexpr uint16_t FILE_CONTENT_FIRST_SECTOR =
    ROOT_DIR_FIRST_SECTOR + ROOT_DIR_SECTOR_COUNT;

/// Special marker for FAT cluster end of file (FAT-16).
static constexpr uint16_t FAT_CLUSTER_END_OF_FILE = 0xFFFF;

#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
/// Maximum length of filename.
/// NOTE: this excludes the period between the filename and extension.
static constexpr uint8_t MAX_FILENAME_LENGTH = 38;
#else
/// Maximum length of filename.
/// NOTE: this excludes the period between the filename and extension.
static constexpr uint8_t MAX_FILENAME_LENGTH = 11;
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES

/// Flag for @ref bios_boot_sector_t boot_sig field indicating that the
/// volume_label and fs_identifier fields are populated.
static constexpr uint8_t BOOT_SIGNATURE_SERIAL_ONLY = 0x28;

/// Flag for @ref bios_boot_sector_t boot_sig field indicating that the
/// volume_label and fs_identifier fields are not populated and should be
/// ignored.
static constexpr uint8_t BOOT_SIGNATURE_SERIAL_LABEL_IDENT = 0x29;

/// Static copy of the bios boot sector that will be presented to the operating
/// system on-demand. Note all fields are in little-endian format.
static bios_boot_sector_t s_bios_boot_sector =
{
    .jump_instruction = {0xEB, 0x3C, 0x90},
    .oem_info = {'M','S','D','O','S','5','.','0'},
    .sector_size = CONFIG_ESPUSB_MSC_VDISK_SECTOR_SIZE,
    .sectors_per_cluster = 1,
    .reserved_sectors = CONFIG_ESPUSB_MSC_VDISK_RESERVED_SECTOR_COUNT,
    .fat_copies = 2,
    .root_directory_entries = CONFIG_ESPUSB_MSC_VDISK_FILE_COUNT,
    .sector_count_16 = CONFIG_ESPUSB_MSC_VDISK_SECTOR_COUNT,
    .media_descriptor = 0xF8,
    .fat_sectors = SECTORS_PER_FAT_TABLE,
    .sectors_per_track = 1,
    .heads = 1,
    .hidden_sectors = 0,
    .sector_count_32 = 0,
    .drive_num = 0x80,
    .reserved = 0,
    .boot_sig = BOOT_SIGNATURE_SERIAL_LABEL_IDENT,
    .volume_serial_number = 0,
    .volume_label = {'e','s','p','3','2','s','2'},
    .fs_identifier = {'F','A','T','1','6',' ',' ',' '},
    .boot_code = {0},
    .signature = {0x55, 0xaa}
};

static std::vector<fat_file_entry_t,
                   PSRAMAllocator<fat_file_entry_t>> s_root_directory;

static uint8_t s_root_directory_entry_usage[ROOT_DIR_SECTOR_COUNT];

static xTimerHandle msc_write_timer;
static constexpr TickType_t TIMER_EXPIRE_TICKS = pdMS_TO_TICKS(1000);
static constexpr TickType_t TIMER_TICKS_TO_WAIT = 0;
static bool msc_write_active = false;
static esp_chip_id_t current_chip_id = ESP_CHIP_ID_INVALID;
static esp_ota_handle_t ota_update_handle = 0;
const esp_partition_t *ota_update_partition = nullptr;
static size_t ota_bytes_received;

static const char * const s_vendor_id = CONFIG_ESPUSB_MSC_VENDOR_ID;
static const char * const s_product_id = CONFIG_ESPUSB_MSC_PRODUCT_ID;
static const char * const s_product_rev = CONFIG_ESPUSB_MSC_PRODUCT_REVISION;

/// Utility function to copy a string into a target field using spaces to pad
/// to a set length.
///
/// @param dst destination array.
/// @param src source string.
/// @param len size of destination array.
static void space_padded_memcpy(char *dst, const char *src, int len)
{
    for (int i = 0; i < len; ++i)
    {
        *dst++ = *src ? *src++ : ' ';
    }
}

/// FreeRTOS Timer expire callback.
///
/// @param pxTimer handle of the timer that expired.
static void msc_write_timeout_cb(xTimerHandle pxTimer)
{
    ESP_LOGV(TAG, "ota_update_timer expired");
    xTimerStop(pxTimer, TIMER_TICKS_TO_WAIT);
    if (ota_update_partition != nullptr && ota_update_handle)
    {
        esp_err_t err =
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_end(ota_update_handle));
        if (err == ESP_OK)
        {
            err = ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_ota_set_boot_partition(ota_update_partition));
        }
        ota_update_end_cb(ota_bytes_received, err);
    }
    ota_update_handle = 0;
    ota_update_partition = nullptr;
    ota_bytes_received = 0;
}

// default implementation.
TU_ATTR_WEAK bool ota_update_start_cb(esp_app_desc_t *app_desc)
{
    return true;
}

// default implementation.
TU_ATTR_WEAK void ota_update_end_cb(size_t received_bytes, esp_err_t err)
{
    ESP_LOGI(TAG, "OTA Update complete callback: %s", esp_err_to_name(err));
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Restarting...");
        esp_restart();
    }
}

// configures the virtual disk system
void configure_virtual_disk(std::string label, uint32_t serial_number)
{
    space_padded_memcpy(s_bios_boot_sector.volume_label, label.c_str(), 11);
    s_bios_boot_sector.volume_serial_number = htole32(serial_number);
    uint32_t sector_count = s_bios_boot_sector.sector_count_16;
    if (sector_count == 0)
    {
        sector_count = s_bios_boot_sector.sector_count_32;
    }
    ESP_LOGI(TAG,
             "USB Virtual disk %-11.11s\n"
             "%d total sectors (%d bytes)\n"
             "%d reserved sector(s)\n"
             "%d sectors per fat (%d bytes)\n"
             "fat0 sector start: %d\n"
             "fat1 sector start: %d\n"
             "root directory sector start: %d (%d entries, %d per sector)\n"
             "first file sector start: %d\n"
#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
             "long filenames: enabled",
#else
             "long filenames: disabled",
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES
             s_bios_boot_sector.volume_label,
             sector_count,
             (sector_count * s_bios_boot_sector.sector_size),
             s_bios_boot_sector.reserved_sectors,
             s_bios_boot_sector.fat_sectors,
             s_bios_boot_sector.fat_sectors * s_bios_boot_sector.sector_size,
             FAT_COPY_0_FIRST_SECTOR,
             FAT_COPY_1_FIRST_SECTOR,
             ROOT_DIR_FIRST_SECTOR,
             CONFIG_ESPUSB_MSC_VDISK_FILE_COUNT,
             DIRENTRIES_PER_SECTOR,
             FILE_CONTENT_FIRST_SECTOR
    );

    // convert fields to little endian
    s_bios_boot_sector.sector_size = htole16(s_bios_boot_sector.sector_size);
    s_bios_boot_sector.reserved_sectors =
        htole16(s_bios_boot_sector.reserved_sectors);
    s_bios_boot_sector.root_directory_entries =
        htole16(s_bios_boot_sector.root_directory_entries);
    s_bios_boot_sector.sector_count_16 =
        htole16(s_bios_boot_sector.sector_count_16);
    s_bios_boot_sector.sector_count_32 =
        htole32(s_bios_boot_sector.sector_count_32);
    s_bios_boot_sector.fat_sectors = htole16(s_bios_boot_sector.fat_sectors);
    s_bios_boot_sector.sectors_per_track =
        htole16(s_bios_boot_sector.sectors_per_track);
    s_bios_boot_sector.heads = htole16(s_bios_boot_sector.heads);
    s_bios_boot_sector.hidden_sectors =
        htole32(s_bios_boot_sector.hidden_sectors);
    s_bios_boot_sector.heads = htole32(s_bios_boot_sector.heads);

    // initialize all root directory sectors to have zero file entries.
    memset(s_root_directory_entry_usage, 0, ROOT_DIR_SECTOR_COUNT);
    // track the volume label as part of the first sector.
    s_root_directory_entry_usage[0] = 1;

    // TODO: remove the usage of FreeRTOS Timer here.
    msc_write_timer =
        xTimerCreate("msc_write_timer", TIMER_EXPIRE_TICKS, pdTRUE, nullptr,
                     msc_write_timeout_cb);
    current_chip_id = ESP_CHIP_ID_INVALID;

    // determine the type of chip that we are currently
    // running and convert it to esp_chip_id_t.
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    if (chip_info.model == CHIP_ESP32S2)
    {
        current_chip_id = ESP_CHIP_ID_ESP32S2;
    }
    else if (chip_info.model == CHIP_ESP32S3)
    {
        current_chip_id = ESP_CHIP_ID_ESP32S3;
    }
}

esp_err_t register_virtual_file(const std::string name, const char *content,
                                uint32_t size, bool read_only,
                                const esp_partition_t *partition)
{
    // one directory entry is reserved for the volume label
    if (s_root_directory.size() > (CONFIG_ESPUSB_MSC_VDISK_FILE_COUNT - 1))
    {
        ESP_LOGE(TAG,
                 "Maximum file count has been reached, rejecting new file!");
        return ESP_ERR_INVALID_STATE;
    }

    fat_file_entry_t file = {};

    // default base name and extension to spaces
    memset(file.name, ' ', TU_ARRAY_SIZE(file.name));
    memset(file.ext, ' ', TU_ARRAY_SIZE(file.ext));

    // break the provided filename into base name and extension
    size_t pos = name.find_first_of('.');
    std::string base_name = name;
    std::string extension = "";
    if (pos == std::string::npos)
    {
        // truncate the filename to the maximum length limit
        if (base_name.length() > MAX_FILENAME_LENGTH)
        {
            base_name.resize(MAX_FILENAME_LENGTH);
        }
        // fill the file name and extension to spaces by default.
        memset(file.name, ' ', 8);
        memset(file.ext, ' ', 3);
        // copy up to 11 characters of the filename into the name field, this
        // will spill over into the ext field.
        memcpy(file.name, base_name.c_str(), std::min((size_t)11
                                                    , base_name.length()));
        // force the file name and extension to be upper case.
        for (size_t index = 0; index < 8; index++)
        {
            file.name[index] = toupper(file.name[index]);
        }
        for (size_t index = 0; index < 3; index++)
        {
            file.ext[index] = toupper(file.ext[index]);
        }
        file.printable_name.assign(std::move(base_name));
    }
    else
    {
        base_name = name.substr(0, pos);
        extension = name.substr(pos + 1, 3);
        // possibly truncate the base name so it fits within the max file length
        if (base_name.length() > MAX_FILENAME_LENGTH - 3)
        {
            base_name.resize(MAX_FILENAME_LENGTH - 3);
        }
        for (size_t index = 0; index < std::min((size_t)8, base_name.length()); index++)
        {
            file.name[index] = toupper(base_name.at(index));
        }
        for (size_t index = 0; index < extension.length(); index++)
        {
            file.ext[index] = toupper(extension.at(index));
        }
        file.printable_name.assign(std::move(base_name)).append(".").append(extension);
    }
#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
    // if the filename is longer than the maximum allowed for 8.3 format
    // convert it to a long filename instead
    if (file.printable_name.length() > 12)
    {
        // mark the file name as truncated.
        file.name[6] = '~';
        file.name[7] = '1';
        uint8_t lfn_checksum = 0;
        uint8_t *p = (uint8_t *)file.name;
        for (size_t index = 11; index; index--)
        {
            lfn_checksum = ((lfn_checksum & 1) << 7) +
                            (lfn_checksum >> 1) + *p++;
        }
        size_t offs = 0;
        uint8_t fragments = 1;
        while (offs < file.printable_name.length())
        {
            fat_long_filename_t part;
            bzero(&part, sizeof(fat_long_filename_t));
            std::string name_part = file.printable_name.substr(offs, 13);
            ESP_LOGD(TAG, "fragment(%d) %s", fragments, name_part.c_str());
            part.sequence = fragments++;
            part.checksum = lfn_checksum;
            part.attributes = (DIRENT_READ_ONLY | DIRENT_HIDDEN |
                               DIRENT_SYSTEM | DIRENT_VOLUME_LABEL);
            // all long filename entries must have one null character
            if (name_part.length() < 13)
            {
                name_part += (unsigned char)0x00;
            }
            // pad remaining characters with 0xFF
            while (name_part.length() < 13)
            {
                name_part += (unsigned char)0xFF;
            }
            // encode the filename parts into the three fields available in the
            // LFN version of the file table entry
            for (size_t idx = 0; idx < name_part.length(); idx++)
            {
                if (idx < 5)
                {
                    part.name[idx] = name_part[idx] != 0xFF ? name_part[idx] : 0xFFFF;
                }
                else if (idx < 11)
                {
                    part.name2[idx - 5] = name_part[idx] != 0xFF ? name_part[idx] : 0xFFFF;
                }
                else
                {
                    part.name3[idx - 11] = name_part[idx] != 0xFF ? name_part[idx] : 0xFFFF;
                }
            }
            // insert the fragment at the front of the collection
            file.lfn_parts.insert(file.lfn_parts.begin(), part);
            offs += name_part.length();
        }
        file.lfn_parts[0].sequence |= 0x40; // mark as last in sequence
        ESP_LOGI(TAG, "Created %d name fragments", file.lfn_parts.size());
    }
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES
    file.content = content;
    file.partition = partition;
    file.size = size;
    file.attributes = DIRENT_ARCHIVE;
    if (read_only)
    {
        file.attributes |= DIRENT_READ_ONLY;
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
    file.end_sector += (size / s_bios_boot_sector.sector_size);
    file.end_cluster = file.start_cluster;
    file.end_cluster += (size / s_bios_boot_sector.sector_size);
    // scan root directory sectors to assign this file to a root dir sector
    for (uint8_t index = 0; index < ROOT_DIR_SECTOR_COUNT; index++)
    {
        uint8_t entries_needed = 1;
#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
        // if the filename is longer than 12 characters (including period)
        // calculate the number of additional entries required. Each fragment
        // can hold up to 13 characters.
        if (file.printable_name.length() > 12)
        {
            // for long filenames we will always need at least one additional
            // entry
            entries_needed++;
            entries_needed += (file.printable_name.length() > 13);
            entries_needed += (file.printable_name.length() > 26);
        }
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES
        if (s_root_directory_entry_usage[index] + entries_needed < DIRENTRIES_PER_SECTOR)
        {
            s_root_directory_entry_usage[index] += entries_needed;
            file.root_dir_sector = index;
            break;
        }
    }
    s_root_directory.push_back(file);
    ESP_LOGI(TAG,
             "File(%s) sectors: %d - %d, clusters: %d - %d, %d bytes, root: %d",
             file.printable_name.c_str(), file.start_sector, file.end_sector,
             file.start_cluster, file.end_cluster, size, file.root_dir_sector);

    return ESP_OK;
}

esp_err_t add_readonly_file_to_virtual_disk(const std::string filename,
                                            const char *content, uint32_t size)
{
    return register_virtual_file(filename, content, size, true, nullptr);
}

esp_err_t add_partition_to_virtual_disk(const std::string partition_name,
                                        const std::string filename,
                                        bool writable)
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
        return register_virtual_file(filename, nullptr, part->size, writable,
                                     part);
    }
    ESP_LOGE(TAG, "Unable to find a partition with name '%s'!"
           , partition_name.c_str());
    return ESP_ERR_NOT_FOUND;
}

// registers the firmware as a file in the virtual disk.
esp_err_t add_firmware_to_virtual_disk(const std::string firmware_name)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part != nullptr)
    {
        const esp_partition_t *part2 =
            esp_ota_get_next_update_partition(nullptr);
        // if there is not a second OTA partition to receive the updated
        // firmware image we need to treat the file as read-only. We do not
        // disable OTA usage here as the read-only flag will disable write.
        bool read_only = (part2 == nullptr || part2 == part);
        return ESP_ERROR_CHECK_WITHOUT_ABORT(
            register_virtual_file(firmware_name, nullptr, part->size,
                                  read_only, part));
    }
    return ESP_ERR_NOT_FOUND;
}

// Utility macro for invoking an ESP-IDF API with with failure return code.
#define ESP_RETURN_ON_ERROR_READ(name, return_code, x)          \
    {                                                           \
        esp_err_t err = ESP_ERROR_CHECK_WITHOUT_ABORT(x);       \
        if (err != ESP_OK)                                      \
        {                                                       \
            ESP_LOGE(TAG, "%s: %s", name, esp_err_to_name(err));\
            return return_code;                                 \
        }                                                       \
    }

// Utility macro for invoking an ESP-IDF API with with failure return code.
#define ESP_RETURN_ON_ERROR_WRITE(name, return_code, x)         \
    {                                                           \
        esp_err_t err = ESP_ERROR_CHECK_WITHOUT_ABORT(x);       \
        if (err != ESP_OK)                                      \
        {                                                       \
            ESP_LOGE(TAG, "%s: %s", name, esp_err_to_name(err));\
            if (ota_update_handle)                              \
            {                                                   \
                ota_update_end_cb(ota_bytes_received, err);     \
                ota_update_partition = nullptr;                 \
                ota_bytes_received = 0;                         \
                ota_update_handle = 0;                          \
            }                                                   \
            return return_code;                                 \
        }                                                       \
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
    memcpy(vendor_id, s_vendor_id, std::min((size_t)8, strlen(s_vendor_id)));
    memcpy(product_id, s_product_id, std::min((size_t)16, strlen(s_product_id)));
    memcpy(product_rev, s_product_rev, std::min((size_t)4, strlen(s_product_rev)));
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
    if (s_bios_boot_sector.sector_count_16)
    {
        *block_count = s_bios_boot_sector.sector_count_16;
    }
    else
    {
        *block_count = s_bios_boot_sector.sector_count_32;
    }
    *block_size  = s_bios_boot_sector.sector_size;
}

// Callback for READ10 command.
// @todo: Ensure reads are bounds checked against bufsize so the MSC buffer can
// be configured via Kconfig.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    bzero(buffer, bufsize);
    if (lba == 0)
    {
        // Requested bios boot sector
        memcpy(buffer, &s_bios_boot_sector, bufsize);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize, MSC_LOG_LEVEL_BOOT_SECTOR);
    }
    else if (lba < ROOT_DIR_FIRST_SECTOR)
    {
        uint32_t fat_table = (lba - FAT_COPY_0_FIRST_SECTOR);
        if (fat_table > s_bios_boot_sector.fat_sectors)
        {
            fat_table -= s_bios_boot_sector.fat_sectors;
        }
        uint32_t cluster_start = fat_table * 256;
        uint32_t cluster_end = ((fat_table + 1) * 256) - 1;
        ESP_LOGD(TAG, "FAT: %d (sector: %d-%d)", fat_table,
                cluster_start, cluster_end);
        uint16_t *buf_16 = (uint16_t *)buffer;
        if (fat_table == 0)
        {
            // cluster zero is reserved for FAT ID and media descriptor.
            buf_16[0] = htole16(0xFF00 | s_bios_boot_sector.media_descriptor);
            // cluster one is reserved.
            buf_16[1] = FAT_CLUSTER_END_OF_FILE;
        }

        for(auto &file : s_root_directory)
        {
            // check if the file is part of this fat cluster
                // A: file start cluster
            // B: file end cluster
            // C: cluster start
            // D: cluster end
            // in_range: A <= D AND B >= C
            if (file.start_cluster <= cluster_end &&
                file.end_cluster >= cluster_start)
            {
                ESP_LOGD(TAG, "File: %s (%d-%d) is in range (%d-%d)"
                       , file.printable_name.c_str(), file.start_cluster
                       , file.end_cluster, cluster_start, cluster_end);
                for(size_t index = 0; index < 256; index++)
                {
                    uint32_t target_cluster = cluster_start + index;
                    // if the target cluster is between start and end cluster
                    // of the file mark it as part of the file.
                    if (target_cluster >= file.start_cluster &&
                        target_cluster <= file.end_cluster)
                    {
                        if (target_cluster != file.end_sector)
                        {
                            buf_16[index] = htole16(target_cluster + 1);
                        }
                        else
                        {
                            buf_16[index] = FAT_CLUSTER_END_OF_FILE;
                        }
                    }
                }
            }
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize, MSC_LOG_LEVEL_FAT_TABLE);
    }
    else if (lba < FILE_CONTENT_FIRST_SECTOR)
    {
        fat_direntry_t *d = static_cast<fat_direntry_t *>(buffer);
        // Requested sector of the root directory
        uint32_t sector_idx = (lba - ROOT_DIR_FIRST_SECTOR);
        ESP_LOGD(TAG, "reading root directory sector %d", sector_idx);
        if (sector_idx == 0)
        {
            ESP_LOGD(TAG, "Adding disk volume label: %11.11s",
                     s_bios_boot_sector.volume_label);
            // NOTE this will overrun d->name and spill over into d->ext
            memcpy(d->name, s_bios_boot_sector.volume_label, 11);
            d->attributes = DIRENT_ARCHIVE | DIRENT_VOLUME_LABEL;
            d->start_cluster = 0;
            d++;
        }
        for (auto &file : s_root_directory)
        {
            if (file.root_dir_sector != sector_idx)
            {
                continue;
            }
            ESP_LOGD(TAG, "Creating directory entry for: %s",
                     file.printable_name.c_str());
#if CONFIG_ESPUSB_MSC_LONG_FILENAMES
            // add directory entries for name fragments.
            if (file.lfn_parts.size())
            {
                fat_long_filename_t *lfn = (fat_long_filename_t *)d;
                for(auto &lfn_part : file.lfn_parts)
                {
                    memcpy(lfn, &lfn_part, sizeof(fat_long_filename_t));
                    lfn++;
                    d++;
                }
            }
#endif // CONFIG_ESPUSB_MSC_LONG_FILENAMES
            // note this will clear the file extension.
            space_padded_memcpy(d->name, file.name, 11);
            space_padded_memcpy(d->ext, file.ext, 3);
            d->attributes = file.attributes;
            d->size = file.size;
            d->start_cluster = file.start_cluster;
            d->create_date = 0x4d99;
            d->update_date = 0x4d99;
            // move to the next directory entry in the buffer
            d++;
        }
        ESP_LOGD(TAG, "Directory entries added: %d",
                 s_root_directory_entry_usage[sector_idx]);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, bufsize,
                                 MSC_LOG_LEVEL_ROOT_DIRECTORY);
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
                    (sector_idx * s_bios_boot_sector.sector_size) + offset;
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
                    ESP_RETURN_ON_ERROR_READ("esp_partition_read", -1,
                        esp_partition_read(file.partition, sector_offset,
                                               buffer, temp_size));
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
        ESP_LOGV(TAG, "Write to BOOT sector");
    }
    else if (lba < ROOT_DIR_FIRST_SECTOR)
    {
        ESP_LOGV(TAG, "Write to FAT cluster chain");
    }
    else if (lba < FILE_CONTENT_FIRST_SECTOR)
    {
        ESP_LOGD(TAG, "write to root directory");
        fat_direntry_t *entry = (fat_direntry_t *)buffer;
        for (uint8_t index = 0; index < DIRENTRIES_PER_SECTOR; index++)
        {
            if (entry->attributes == 0x0F && entry->start_cluster == 0)
            {
                // long filename entry will always have attributes set to 0x0F
                // and starting cluster as zero.
                fat_long_filename_t *lfn = (fat_long_filename_t*)entry;
                uint8_t name[13] = {0};
                for (uint8_t idx = 0; idx < 13; idx++)
                {
                    uint8_t ch = '\0';
                    if (idx < 5 && (le16toh(lfn->name[idx]) & 0xFF) != 0xFF)
                    {
                        ch = (le16toh(lfn->name[idx]) & 0xFF);
                    }
                    else if (idx < 11 && (le16toh(lfn->name2[idx - 5]) & 0xFF) != 0xFF)
                    {
                        ch = (le16toh(lfn->name2[idx - 5]) & 0xFF);
                    }
                    else if (idx < 13 && (le16toh(lfn->name3[idx - 11]) & 0xFF) != 0xFF)
                    {
                        ch = (le16toh(lfn->name3[idx - 11]) & 0xFF);
                    }
                    name[idx] = ch;
                }
                ESP_LOGI(TAG, "LFN: idx:%d (last:%d) %13.13s",
                         (lfn->sequence & 0x1F),
                         (lfn->sequence & 0x40) == 0x40, name);
            }
            else if (entry->start_cluster)
            {
                ESP_LOGI(TAG, "File: %8.8s.%3.3s, size: %d", entry->name, entry->ext, entry->size);
            }
            entry++;
        }
        // @todo add callback for file received
    }
    else
    {
        // check if this is the first write of a new file.
        if (!msc_write_active)
        {
            // If the first byte received in the buffer is recognized as the
            // esp magic byte, try and validate the data as a valid application
            // image.
            if (buffer[0] == ESP_IMAGE_HEADER_MAGIC)
            {
                // the first segment of the received binary should have the
                // image header, segment header and app description. These are
                // used as a first pass validation of the received data to
                // ensure it is a valid ESP application image.
                esp_image_header_t *image = (esp_image_header_t *)buffer;
                esp_app_desc_t *app_desc =
                    (esp_app_desc_t *)(buffer + sizeof(esp_image_header_t) +
                                       sizeof(esp_image_segment_header_t));
                // validate the image magic byte and chip type to
                // ensure it matches the currently running chip.
                if (image->magic == ESP_IMAGE_HEADER_MAGIC &&
                    image->chip_id != ESP_CHIP_ID_INVALID &&
                    image->chip_id == current_chip_id &&
                    app_desc->magic_word == ESP_APP_DESC_MAGIC_WORD)
                {
                    ESP_LOGI(TAG, "Received data appears to be firmware:");
                    ESP_LOGI(TAG, "Name: %s (%s)",
                             app_desc->project_name, app_desc->version);
                    ESP_LOGI(TAG, "ESP-IDF version: %s", app_desc->idf_ver);
                    ESP_LOGI(TAG, "Compile timestamp: %s %s", app_desc->date,
                             app_desc->time);
                    if (!ota_update_start_cb(app_desc))
                    {
                        ESP_LOGE(TAG, "OTA update rejected by application.");
                        return -1;
                    }
                    // it appears to be a firmware, try and find a place to
                    // write it to
                    ota_update_partition =
                        esp_ota_get_next_update_partition(NULL);
                    if (ota_update_partition == nullptr ||
                        ota_update_partition == esp_ota_get_running_partition())
                    {
                        ESP_LOGE(TAG, "Unable to locate a free OTA partition.");
                        return -1;
                    }
                    ESP_LOGI(TAG, "Attempting to start OTA image");
                    ESP_RETURN_ON_ERROR_WRITE("esp_ota_begin", -1,
                        esp_ota_begin(ota_update_partition, OTA_SIZE_UNKNOWN,
                                        &ota_update_handle));
                    ESP_LOGV(TAG, "ota_update_handle:%d", ota_update_handle);
                }
            }
            else
            {
                // doesn't appear to be a firmware image, allocate a buffer in
                // PSRAM (if available) to store the data as it arrives until
                // the root directory has been updated to map to a filename.
            }

            // track that we are actively receiving data
            msc_write_active = true;
        }
        // if we are actively writing an ota update process it immediately.
        if (ota_update_handle)
        {
            ESP_RETURN_ON_ERROR_WRITE("esp_ota_write", -1,
                esp_ota_write(ota_update_handle, buffer, bufsize));
            // track how much has been written
            ota_bytes_received += bufsize;
        }
        else
        {
            // send the data to the temp buffer
        }

        // restart the update timer
        xTimerChangePeriod(msc_write_timer, TIMER_EXPIRE_TICKS,
                            TIMER_TICKS_TO_WAIT);
        if (!xTimerIsTimerActive(msc_write_timer) &&
            xTimerStart(msc_write_timer, TIMER_TICKS_TO_WAIT) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to restart MSC timer, giving up!");

            if (ota_update_handle)
            {
                ota_update_end_cb(ota_bytes_received, ESP_FAIL);
            }

            // reset state so that the timer expire callback does not try to
            // use the received data.
            ota_update_partition = nullptr;
            ota_bytes_received = 0;
            ota_update_handle = 0;
            return -1;
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

#endif // CONFIG_ESPUSB_MSC