// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
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

#pragma once

#include "sdkconfig.h"
#include "tusb_config.h"
#include "tusb.h"

#include <string>

/// USB Descriptor string indexes.
typedef enum
{
    /// This is used for the USB Device Manufacturer string.
    USB_DESC_MANUFACTURER = 1,

    /// This is used for the USB Product string.
    USB_DESC_PRODUCT,
    
    /// This is used for the USB Product string.
    USB_DESC_SERIAL_NUMBER,
    
    /// This is used for the USB CDC Device Description string.
    USB_DESC_CDC,
    
    /// This is used for the USB Mass Storage Device Description string.
    USB_DESC_MSC,

    /// This is used for the USB Human Interface Device Description string.
    USB_DESC_HID,

    /// This is used for the USB Vendor Device Description string.
    USB_DESC_VENDOR,

    /// This is used for the USB MIDI Device Description string.
    USB_DESC_MIDI,

    /// This is used internally and will be ignored by callers if used.
    USB_DESC_MAX_COUNT
} esp_usb_descriptor_index_t;

/// USB Device Endpoint assignments.
///
/// NOTE: Most device drivers use two endpoints with the second endpoint being
/// the same as below but ORed with 0x80. The CDC device driver also uses the
/// @ref ENDPOINT_NOTIF.
typedef enum
{
    /// CDC endpoint.
    ENDPOINT_CDC = 0x02,

    /// Mass Storage endpoint.
    ENDPOINT_MSC = 0x03,

    /// MIDI endpoint.
    ENDPOINT_MIDI = 0x05,

    /// Vendor endpoint.
    ENDPOINT_VENDOR = 0x06,

    /// Notification endpoint.
    ENDPOINT_NOTIF = 0x81,
} esp_usb_endpoint_t;

#if CONFIG_TINYUSB_HID_ENABLED
/// USB HID device report types.
typedef enum
{
    /// The reported event is from a keyboard.
    REPORT_ID_KEYBOARD = 1,

    /// The reported event is from a mouse.
    REPORT_ID_MOUSE
} esp_usb_hid_report_t;
#endif

typedef enum
{
#if CONFIG_TINYUSB_CDC_ENABLED
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
#endif
#if CONFIG_TINYUSB_MSC_ENABLED
    ITF_NUM_MSC,
#endif
#if CONFIG_TINYUSB_HID_ENABLED
    ITF_NUM_HID,
#endif
#if CONFIG_TINYUSB_MIDI_ENABLED
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
#endif
#if CONFIG_TINYUSB_VENDOR_ENABLED 
    ITF_NUM_VENDOR,
#endif
    ITF_NUM_TOTAL
} esp_usb_interface_t;

/// Initializes the USB peripheral and prepares the default descriptors.
///
/// @param external_phy should be left as false.
void init_usb_subsystem(bool external_phy = false);

/// Creates a background task for TinyUSB processing of USB packets.
///
/// NOTE: The task uses 4096 bytes for the stack and runs at the TCP/IP task
/// priority.
void start_usb_task();

/// Configures the USB descriptor.
///
/// @param desc when not null will replace the default descriptor.
/// @param version will set the bcdDevice value of the default descriptor.
///
/// NOTE: When desc is not null the version will be ignored.
void configure_usb_descriptor(tusb_desc_device_t *desc,
                              uint16_t version = 0x0000);

/// Configures a USB descriptor string.
///
/// @param index is the @ref esp_usb_descriptor_index_t for the string being
/// configured.
/// @param value is the value to assign to the descriptor string.
///
/// NOTE: USB descriptor strings are limited to a maximum of 31 ASCII
/// characters.
void configure_usb_descriptor_str(esp_usb_descriptor_index_t index,
                                  const char *value);

/// Configures a 4MB virtual disk.
///
/// @param label will be used as the disk label that may be displayed by the
/// operating system.
/// @param serial_number will be used as the disk serial number.
///
/// NOTE: The disk label is limited to 11 ASCII characters and will be
/// truncated if necessary.
void configure_virtual_disk(std::string label, uint32_t serial_number);

/// Adds a file to the virtual disk that is read-only.
///
/// @param filename is the name of the file on the virtual disk.
/// @param content is the raw byte content for the file.
/// @param size is the number of bytes in the file.
///
/// NOTE: filename is limited to 8.3 format and will be truncated if
/// necessary. If the filename provided does not have a "." character then it
/// will be used as-is up to 11 ASCII characters.
void add_readonly_file_to_virtual_disk(const std::string filename,
                                       const char *content, uint32_t size);

/// Exposes a partition as a file on the virtual disk.
///
/// @param partition_name is the name of the partition to convert to a file.
/// @param filename is the name of the file on the virtual disk.
/// @param writable controls if the file can be written to over USB.
///
/// NOTE: filename is limited to 8.3 format and will be truncated if
/// necessary. If the filename provided does not have a "." character then it
/// will be used as-is up to 11 ASCII characters.
void add_partition_to_virtual_disk(const std::string partition_name,
                                   const std::string filename,
                                   bool writable = false);

/// Adds the currently running firmware and the next available OTA slot as the
/// previous firmware.
///
/// @param current_name is used as the filename for the currently running
/// firmware.
/// @param previous_name is used as the filename for the previous firmware.
///
/// NOTE: Both current_name and previous_name are limited to 8.3 format and
/// will be truncated if necessary. If the filename does not have a "."
/// character then it will be used as-is up to 11 ASCII characters.
///
/// NOTE: This API expects only ota_0 and ota_1 to be defined in the partition
/// map. A future revision may adjust this to support more than two OTA
/// partitions.
void add_firmware_to_virtual_disk(const std::string current_name, const std::string previous_name);