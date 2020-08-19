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

enum esp_usb_endpoint_t
{
    ENDPOINT_CDC = 0x02,
    ENDPOINT_MSC = 0x03,
    ENDPOINT_MIDI = 0x05,
    ENDPOINT_VENDOR = 0x06,
    ENDPOINT_NOTIF = 0x81,
};

enum esp_usb_descriptor_index_t
{
    USB_DESC_MANUFACTURER = 1,
    USB_DESC_PRODUCT,
    USB_DESC_SERIAL_NUMBER,
    USB_DESC_CDC,
    USB_DESC_MSC,
    USB_DESC_HID,
    USB_DESC_VENDOR,
    USB_DESC_MIDI,
    USB_DESC_MAX_COUNT
};

#if CONFIG_TINYUSB_HID_ENABLED
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE
} esp_usb_hid_report_t;
#endif

enum esp_usb_interface_t
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
};

void init_usb_subsystem(bool external_phy = false);
void start_usb_task();
void configure_usb_descriptor(tusb_desc_device_t *desc, uint16_t bcdDevice = 0x0000);
void configure_usb_descriptor_str(esp_usb_descriptor_index_t index, const char *value);

void configure_virtual_disk(std::string label, uint32_t serial_number);
void add_readonly_file_to_virtual_disk(const std::string name, const char *content, uint32_t size);