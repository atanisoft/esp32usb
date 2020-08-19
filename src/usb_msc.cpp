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

#if CONFIG_TINYUSB_MSC_ENABLED

#include <esp_log.h>
#include <endian.h>

static constexpr const char * const TAG = "USB:MSC";

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
    uint8_t reserved2[512 - 62 - 2];        // 510
    uint8_t filesystem_marker[2];           // 512 -- boot sector signature
} fat_bootblock_t;

static_assert(sizeof(fat_bootblock_t) == 512,
              "fat_bootblock_t should be 512 bytes");

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

enum FAT_DIRENTRY_ATTRS
{
    READ_ONLY = 0x01,
    HIDDEN = 0x02,
    SYSTEM = 0x04,
    VOLUME_LABEL = 0x08,
    SUB_DIRECTORY = 0x10,
    ARCHIVE = 0x20,
    DEVICE = 0x40,
    RESERVED = 0x80
};

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
} fat_file_entry_t;

static const uint16_t DISK_SECTOR_SIZE = 512;
static const uint16_t FAT_BLOCK_COUNT = 4200;
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

static fat_bootblock_t s_boot_block =
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
    .filesystem_marker = {0x55, 0xaa}
};
static size_t s_root_entry_count = 0;
static fat_file_entry_t s_root_directory[64] = {};

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
    space_padded_memcpy(s_boot_block.volume_label, label.c_str(), 11);
    s_boot_block.volume_serial_number = htole32(serial_number);
    ESP_LOGI(TAG,
             "USB Virtual disk %-11.11s\n"
             "%d total sectors (%d bytes)\n"
             "%d reserved sector(s)\n"
             "%d sectors per FAT block (%d bytes)\n"
             "fat0 sector start: %d\n"
             "fat1 sector start: %d\n"
             "root directory sector start: %d (%d entries, %d per sector)\n"
             "first file sector start: %d\n"
           , s_boot_block.volume_label
           , s_boot_block.total_sectors_16
           , s_boot_block.total_sectors_16 * s_boot_block.sector_size
           , s_boot_block.reserved_sectors
           , s_boot_block.sectors_per_fat
           , s_boot_block.sectors_per_fat * s_boot_block.sector_size
           , FAT_COPY_0_FIRST_SECTOR
           , FAT_COPY_1_FIRST_SECTOR
           , ROOT_DIR_FIRST_SECTOR
           , ROOT_DIRECTORY_ENTRIES
           , FILE_CONTENT_FIRST_SECTOR
           , DIRENTRIES_PER_SECTOR
    );
    // convert fields to little endian
    s_boot_block.sector_size = htole16(s_boot_block.sector_size);
    s_boot_block.reserved_sectors = htole16(s_boot_block.reserved_sectors);
    s_boot_block.root_directory_entries = htole16(s_boot_block.root_directory_entries);
    s_boot_block.total_sectors_16 = htole16(s_boot_block.total_sectors_16);
    s_boot_block.sectors_per_fat = htole16(s_boot_block.sectors_per_fat);
    s_boot_block.sectors_per_track = htole16(s_boot_block.sectors_per_track);
    s_boot_block.heads = htole16(s_boot_block.heads);
    s_boot_block.hidden_sectors = htole32(s_boot_block.hidden_sectors);
    s_boot_block.heads = htole32(s_boot_block.heads);
}

void register_virtual_file(const std::string name, const char *content,
                           uint32_t size, bool read_only)
{
    // zero out the file descriptor block
    bzero(&s_root_directory[s_root_entry_count], sizeof(fat_file_entry_t));

    // default base name and extension to spaces
    memset(s_root_directory[s_root_entry_count].name, ' ',
           TU_ARRAY_SIZE(s_root_directory[s_root_entry_count].name));
    memset(s_root_directory[s_root_entry_count].ext, ' ',
           TU_ARRAY_SIZE(s_root_directory[s_root_entry_count].ext));

    // break the provided filename into base name and extension
    size_t pos = name.find_first_of('.');
    if (pos == std::string::npos)
    {
        // copy the filename as-is into the base name and extension fields with
        // space padding as needed. NOTE: this will overflow the name field.
        strncpy(s_root_directory[s_root_entry_count].name, name.c_str(),
            name.length() > 11 ? 11 : name.length());
    }
    else
    {
        std::string base_name = name.substr(0, pos);
        std::string extension = name.substr(pos + 1, 3);
        // truncate the base name to a max of eight characters
        if (base_name.length() > 8)
        {
            base_name.resize(8);
        }
        strncpy(s_root_directory[s_root_entry_count].name, base_name.c_str(),
                base_name.length());
        strncpy(s_root_directory[s_root_entry_count].ext, extension.c_str(),
                extension.length());
    }
    s_root_directory[s_root_entry_count].content = content;
    s_root_directory[s_root_entry_count].size = size;
    s_root_directory[s_root_entry_count].flags = FAT_DIRENTRY_ATTRS::ARCHIVE;
    if (read_only)
    {
        s_root_directory[s_root_entry_count].flags |= FAT_DIRENTRY_ATTRS::READ_ONLY;
    }

    if (s_root_entry_count)
    {
        s_root_directory[s_root_entry_count].start_sector =
            s_root_directory[s_root_entry_count-1].end_sector + 1;
        s_root_directory[s_root_entry_count].start_cluster =
            s_root_directory[s_root_entry_count - 1].end_cluster + 1;
    }
    else
    {
        s_root_directory[s_root_entry_count].start_sector =
            FILE_CONTENT_FIRST_SECTOR;
        s_root_directory[s_root_entry_count].start_cluster = 2;
    }
    s_root_directory[s_root_entry_count].end_sector =
        s_root_directory[s_root_entry_count].start_sector;
    s_root_directory[s_root_entry_count].end_sector +=
        (size / DISK_SECTOR_SIZE);
    s_root_directory[s_root_entry_count].end_cluster =
        s_root_directory[s_root_entry_count].start_cluster;
    s_root_directory[s_root_entry_count].end_cluster +=
        (size / DISK_SECTOR_SIZE);
    ESP_LOGI(TAG, "MSC(%zu) %s.%s(%d) sectors: %d - %d, clusters: %d - %d",
             s_root_entry_count,
             s_root_directory[s_root_entry_count].name,
             s_root_directory[s_root_entry_count].ext, size,
             s_root_directory[s_root_entry_count].start_sector,
             s_root_directory[s_root_entry_count].end_sector,
             s_root_directory[s_root_entry_count].start_cluster,
             s_root_directory[s_root_entry_count].end_cluster);
    // move to next entry
    s_root_entry_count++;
}

void add_readonly_file_to_virtual_disk(const std::string name,
                                       const char *content, uint32_t size)
{
    register_virtual_file(name, content, size, true);
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
    *block_count = FAT_BLOCK_COUNT;
    *block_size  = DISK_SECTOR_SIZE;
}

// Callback for READ10 command.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    bzero(buffer, bufsize);
    if (lba == 0)
    {
        // Requested boot block
        memcpy(buffer, &s_boot_block, sizeof(fat_bootblock_t));
        TU_LOG2_MEM(buffer, bufsize, 0);
    }
    else if (lba < ROOT_DIR_FIRST_SECTOR)
    {
        ESP_LOGV(TAG, "sector map: %d (%d)", lba, ROOT_DIR_FIRST_SECTOR);
        uint32_t target_sector = (lba - FAT_COPY_0_FIRST_SECTOR);
        if (target_sector > SECTORS_PER_FAT_BLOCK)
        {
            // second fat sector block is identical to first
            target_sector -= SECTORS_PER_FAT_BLOCK;
            ESP_LOGV(TAG, "Second FAT sector map: %d", target_sector);
        }
        if (target_sector == 0)
        {
            ESP_LOGV(TAG, "Generating FAT sector map");
            // requesting sector map
            size_t offs = 0;
            uint16_t *buf = (uint16_t *)buffer;
            buf[offs++] = htole16(0xFFF0);
            buf[offs++] = htole16(0xFFFF);
            // add entries for each defined file.
            for(size_t index = 0; index < s_root_entry_count; index++)
            {
                ESP_LOGV(TAG,
                         "File(%zu) %s.%s, offs: %zu, first sector: %d (%02x), "
                         "last sector: %d(%02x)",
                         index, s_root_directory[index].name, s_root_directory[index].ext, offs << 1,
                         s_root_directory[index].start_sector, s_root_directory[index].start_sector,
                         s_root_directory[index].end_sector, s_root_directory[index].end_sector);
                // if the starting and ending sector is not the same we need to add more entries
                // to the sector map.
                if (s_root_directory[index].start_cluster != s_root_directory[index].end_cluster)
                {
                    for (uint16_t sector_index = s_root_directory[index].start_cluster;
                        sector_index < s_root_directory[index].end_cluster;
                        sector_index++)
                    {
                        buf[offs++] = htole16(sector_index + 1);
                    }
                }
                // end of file marker
                buf[offs++] = htole16(0xFFFF);
            }
            TU_LOG2_MEM(buffer, bufsize, 0);
        }
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
            memcpy(d->name, s_boot_block.volume_label, 11);
            d->attrs = FAT_DIRENTRY_ATTRS::ARCHIVE | FAT_DIRENTRY_ATTRS::VOLUME_LABEL;
            // move beyond the first entry
            d++;
            for(size_t index = 0; index < s_root_entry_count; index++, d++)
            {
                // note this will clear the file extension.
                space_padded_memcpy(d->name, s_root_directory[index].name, 11);
                space_padded_memcpy(d->ext, s_root_directory[index].ext, 3);
                d->attrs = s_root_directory[index].flags;
                d->size = s_root_directory[index].size;
                d->start_cluster = s_root_directory[index].start_cluster;
                d->create_date = 0x4d99;
                d->update_date = 0x4d99;
            }
        }
        TU_LOG2_MEM(buffer, bufsize, 0);
    }
    else
    {
        uint32_t sector_idx = lba;
        // Requested sector from file space
        // scan the root directory entries for a file that is in the requested sector.
        for(size_t index = 0; index < s_root_entry_count; index++)
        {
            if (sector_idx >= s_root_directory[index].start_sector &&
                sector_idx <= s_root_directory[index].end_sector)
            {
                ESP_LOGV(TAG, "MSC-FILE(%d:%d [%d:%d]) name:%s.%s size:%d",
                         index, sector_idx,
                         s_root_directory[index].start_sector,
                         s_root_directory[index].end_sector,
                         s_root_directory[index].name,
                         s_root_directory[index].ext,
                         s_root_directory[index].size);
                // found file
                if (s_root_directory[index].size < DISK_SECTOR_SIZE &&
                    bufsize > s_root_directory[index].size)
                {
                    ESP_LOGV(TAG, "reading full file into single buffer");
                    memcpy(buffer, s_root_directory[index].content,
                           s_root_directory[index].size);
                }
                else
                {
                    sector_idx -= s_root_directory[index].start_sector;
                    size_t temp_size = bufsize;
                    size_t offset = sector_idx * DISK_SECTOR_SIZE;
                    if (temp_size > (s_root_directory[index].size - offset))
                    {
                        temp_size = s_root_directory[index].size - offset;
                    }
                    ESP_LOGV(TAG,
                             "[%d:%d] reading %zu (%zu) bytes from offset:%zu",
                             s_root_directory[index].start_sector, sector_idx,
                             temp_size, bufsize, offset);
                    memcpy(buffer, s_root_directory[index].content + offset,
                           temp_size);
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
        ESP_LOGV(TAG, "Write to sector map\n");
    }
    else if (lba < s_root_directory[0].start_sector)
    {
        ESP_LOGV(TAG, "write to root directory\n");
    }
    else
    {
        // scan the root directory entries for a file that is in the requested
        // sector. Check if it is flagged as read-only and reject the write
        // attempt.
        for(size_t index = 0; index < s_root_entry_count; index++)
        {
            if (lba >= s_root_directory[index].start_sector &&
                lba <= s_root_directory[index].end_sector)
            {
                if (s_root_directory[index].flags & FAT_DIRENTRY_ATTRS::READ_ONLY)
                {
                    ESP_LOGV(TAG, "Attempt to write to read only file.");
                    return -1;
                }
                uint8_t *target = (uint8_t *)s_root_directory[index].content;
                target += (lba * s_boot_block.sector_size) + offset;
                memcpy(target, buffer, bufsize);
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