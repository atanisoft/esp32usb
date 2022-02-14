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

#include "sdkconfig.h"

// if Esp32USB debug is enabled set the local log level higher than any of the
// pre-defined log levels.
#if CONFIG_ESPUSB_DEBUG
#define LOG_LOCAL_LEVEL 0xFF
#endif

#if CONFIG_ESPUSB_HID

#include <class/hid/hid.h>

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
TU_ATTR_WEAK uint16_t tud_hid_get_report_cb(
    uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
    uint8_t *buffer, uint16_t reqlen)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
TU_ATTR_WEAK void tud_hid_set_report_cb(
    uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
    uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

uint8_t const desc_hid_keyboard_report[] =
{
    TUD_HID_REPORT_DESC_KEYBOARD()
};

uint8_t const desc_hid_mouse_report[] =
{
    TUD_HID_REPORT_DESC_MOUSE()
};

uint8_t const desc_hid_consumer_report[] =
{
    TUD_HID_REPORT_DESC_CONSUMER()
};

uint8_t const desc_hid_gamepad_report[] =
{
    TUD_HID_REPORT_DESC_GAMEPAD()
};

// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
TU_ATTR_WEAK uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch(instance)
    {
        case 0:
            return desc_hid_keyboard_report;
        case 1:
            return desc_hid_mouse_report;
        case 2:
            return desc_hid_consumer_report;
        case 3:
            return desc_hid_gamepad_report;
    }
    return nullptr;
}

#endif // CONFIG_ESPUSB_HID
