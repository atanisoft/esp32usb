// Copyright 2020 Espressif Systems (Shanghai) Co. Ltd.
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
    /// No device is connected.
    LINE_STATE_DISCONNECTED,

    /// A device is connected.
    LINE_STATE_CONNECTED,

    /// This state is reached by deasserting DTR and RTS asserted and is the
    /// first step used by esptool.py to enter download mode.
    LINE_STATE_MAYBE_ENTER_DOWNLOAD_DTR,

    /// This state is reached by asserting both DTR and RTS. This normally will
    /// happen when a device is connected to the USB port. It is also the
    /// second state used by esptool.py to enter download mode.
    LINE_STATE_MAYBE_CONNECTED,

    /// This state is reached by asserting DTR and deasserting RTS. This is the
    /// third step used by esptool.py to enter download mode.
    LINE_STATE_MAYBE_ENTER_DOWNLOAD_RTS,

    /// This state is used by the usb shutdown hook to trigger a restart into
    /// esptool binary download mode.
    ///
    /// NOTE: This is not the same as DFU download mode.
    LINE_STATE_REQUEST_DOWNLOAD,

    /// This state is used by the usb shutdown hook to trigger a restart into
    /// DFU download mode.
    LINE_STATE_REQUEST_DOWNLOAD_DFU
} esp_line_state_t;

/// Initializes the USB peripheral and prepares the default descriptors.
///
/// @param external_phy should be left as false.
void init_usb_subsystem(bool external_phy = false);

/// Creates a background task for TinyUSB processing of USB packets.
///
/// NOTE: The task uses 4096 bytes for the stack and runs at the TCP/IP task
/// priority.
void start_usb_task();

/// Writes a buffer to the USB CDC if there is a device connected.
///
/// @param buf is the buffer to send.
/// @param size is the size of the buffer.
///
/// @return the number of bytes transmitted.
size_t write_to_cdc(const char *buf, size_t size);

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
/// NOTE: USB descriptor strings only support ASCII characters at this time and
/// have a maximum length of 126 characters.
void configure_usb_descriptor_str(esp_usb_descriptor_index_t index,
                                  const char *value);

/// Requests that the next time the system restarts it should be started in DFU
/// mode.
///
/// NOTE: If a USB device connects or disconnects after this call has been made
/// and the system has not restarted this request will be discarded.
void request_dfu_mode();

/// Callback for CDC line state change for application code.
///
/// @param status is the new @ref esp_line_state_t state.
/// @param download_mode_requested will be set to true if there has been a
/// request to restart the system into download mode (esptool or DFU).
///
/// @return The callback function should return true if the USB code should
/// make the call to @ref esp_restart() internally. If the application needs
/// to prepare for restart it should return false and schedule the restart as
/// soon as possible after this function returns.
///
/// NOTE: The return value from this callback function is only used when there
/// has been a request to restart into download mode and
/// download_mode_requested is true.
bool usb_line_state_changed_cb(esp_line_state_t status,
                               bool download_mode_requested);

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
/// @return ESP_OK if the file was successfully added to the virtual disk or
/// ESP_ERR_INVALID_STATE if there are too many files on the virtual disk.
///
/// NOTE: filename is limited to 8.3 format and will be truncated if
/// necessary. If the filename provided does not have a "." character then it
/// will be used as-is up to 11 ASCII characters.
esp_err_t add_readonly_file_to_virtual_disk(const std::string filename,
                                            const char *content,
                                            uint32_t size);

/// Exposes a partition as a file on the virtual disk.
///
/// @param partition_name is the name of the partition to convert to a file.
/// @param filename is the name of the file on the virtual disk.
/// @param writable controls if the file can be written to over USB.
///
/// @return ESP_OK if the file was successfully added to the virtual disk or
/// ESP_ERR_INVALID_STATE if there are too many files on the virtual disk or
/// ESP_ERR_NOT_FOUND if the partition could not be found.
///
/// NOTE: filename is limited to 8.3 format and will be truncated if
/// necessary. If the filename provided does not have a "." character then it
/// will be used as-is up to 11 ASCII characters.
esp_err_t add_partition_to_virtual_disk(const std::string partition_name,
                                        const std::string filename,
                                        bool writable = false);

/// Adds the currently running firmware and the next available OTA slot as the
/// previous firmware.
///
/// @param current_name is used as the filename for the currently running
/// firmware, note that this parameter is optional and when omitted the
/// filename will be "firmware.bin".
/// @param previous_name is used as the filename for the previous firmware,
/// note that this parameter is optional and when omitted only the currently
/// running firmware will be present on the virtual disk.
///
/// @return ESP_OK if the file was successfully added to the virtual disk or
/// ESP_ERR_INVALID_STATE if there are too many files on the virtual disk.
///
/// NOTE: Both current_name and previous_name are limited to 8.3 format and
/// will be truncated if necessary. If the filename does not have a "."
/// character then it will be used as-is up to 11 ASCII characters.
///
/// NOTE: This API expects only ota_0 and ota_1 to be defined in the partition
/// map. A future revision may adjust this to support more than two OTA
/// partitions.
esp_err_t add_firmware_to_virtual_disk(const std::string current_name = "firmware.bin",
                                       const std::string previous_name = "");