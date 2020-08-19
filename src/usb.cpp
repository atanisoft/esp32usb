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

#include <driver/gpio.h>
#include <driver/periph_ctrl.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/usb_hal.h>
#include <soc/gpio_periph.h>
#include <soc/usb_periph.h>
#include <string>
#include "usb.h"

static constexpr const char * const TAG = "USB";

void init_usb_subsystem(bool external_phy)
{
    ESP_LOGV(TAG, "Initializing USB peripheral");
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_enable(PERIPH_USB_MODULE);

    usb_hal_context_t hal;
    hal.use_external_phy = external_phy;
    ESP_LOGV(TAG, "Initializing USB HAL");
    usb_hal_init(&hal);

    for (const usb_iopin_dsc_t *iopin = usb_periph_iopins; iopin->pin != GPIO_NUM_NC; ++iopin)
    {
        if (external_phy || iopin->ext_phy_only == 0)
        {
            esp_rom_gpio_pad_select_gpio((gpio_num_t)iopin->pin);
            if (iopin->is_output)
            {
                ESP_LOGV(TAG, "Configuring USB GPIO %d as OUTPUT", iopin->pin);
                esp_rom_gpio_connect_out_signal((gpio_num_t)iopin->pin, iopin->func, false, false);
            }
            else
            {
                esp_rom_gpio_connect_in_signal((gpio_num_t)iopin->pin, iopin->func, false);
                if ((iopin->pin != GPIO_MATRIX_CONST_ZERO_INPUT) && (iopin->pin != GPIO_MATRIX_CONST_ONE_INPUT))
                {
                    ESP_LOGV(TAG, "Configuring USB GPIO %d as INPUT", iopin->pin);
                    gpio_set_direction((gpio_num_t)iopin->pin, GPIO_MODE_INPUT);
                }
            }
            esp_rom_gpio_pad_unhold((gpio_num_t)iopin->pin);
        }
    }
    if (!external_phy)
    {
        ESP_LOGV(TAG, "Setting GPIO %d drive to %d", USBPHY_DM_NUM, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)USBPHY_DM_NUM, (gpio_drive_cap_t)GPIO_DRIVE_CAP_3);
        ESP_LOGV(TAG, "Setting GPIO %d drive to %d", USBPHY_DP_NUM, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)USBPHY_DP_NUM, (gpio_drive_cap_t)GPIO_DRIVE_CAP_3);
    }
    ESP_LOGV(TAG, "USB system initialized");
}

static void usb_device_task(void *param)
{
    ESP_LOGI(TAG, "Starting TinyUSB stack");

    ESP_ERROR_CHECK(tusb_init());

    while (1)
    {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static constexpr uint32_t USB_TASK_STACK_SIZE = 4096L;
static constexpr UBaseType_t USB_TASK_PRIORITY = ESP_TASK_TCPIP_PRIO;

void start_usb_task()
{
    BaseType_t res =
        xTaskCreate(usb_device_task, "esp-usb", USB_TASK_STACK_SIZE, nullptr,
                    USB_TASK_PRIORITY, nullptr);
    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create USB task!");
        abort();
    }
}

#define _PID_MAP(itf, n)  ((CFG_TUD_##itf) << (n))

// When CDC is enabled set the default descriptor to use ACM mode.
#if CONFIG_TINYUSB_CDC_ENABLED
#define USB_DEVICE_CLASS TUSB_CLASS_MISC
#define USB_DEVICE_SUBCLASS MISC_SUBCLASS_COMMON
#define USB_DEVICE_PROTOCOL MISC_PROTOCOL_IAD
#else
#define USB_DEVICE_CLASS 0x00
#define USB_DEVICE_SUBCLASS 0x00
#define USB_DEVICE_PROTOCOL 0x00
#endif

static tusb_desc_device_t s_descriptor =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = USB_DEVICE_CLASS,
    .bDeviceSubClass    = USB_DEVICE_SUBCLASS,
    .bDeviceProtocol    = USB_DEVICE_PROTOCOL,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_TINYUSB_USB_VENDOR_ID,
    .idProduct          = (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) |
                           _PID_MAP(HID, 2) | _PID_MAP(MIDI, 3) |
                           _PID_MAP(VENDOR, 4)),
    .bcdDevice          = CONFIG_TINYUSB_DESC_BCDDEVICE,
    .iManufacturer      = USB_DESC_MANUFACTURER,
    .iProduct           = USB_DESC_PRODUCT,
    .iSerialNumber      = USB_DESC_SERIAL_NUMBER,
    .bNumConfigurations = 0x01
};

static constexpr uint16_t USB_DESCRIPTORS_CONFIG_TOTAL_LEN =
    TUD_CONFIG_DESC_LEN +
    (CONFIG_TINYUSB_CDC_ENABLED * TUD_CDC_DESC_LEN) +
    (CONFIG_TINYUSB_MSC_ENABLED * TUD_MSC_DESC_LEN) +
    (CONFIG_TINYUSB_HID_ENABLED * TUD_HID_DESC_LEN) +
    (CONFIG_TINYUSB_VENDOR_ENABLED * TUD_VENDOR_DESC_LEN) +
    (CONFIG_TINYUSB_MIDI_ENABLED * TUD_MIDI_DESC_LEN);

uint8_t const desc_configuration[] =
{
    // interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_DESCRIPTORS_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          CONFIG_TINYUSB_MAX_POWER_USAGE),
#if CONFIG_TINYUSB_CDC_ENABLED
    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, USB_DESC_CDC, ENDPOINT_NOTIF,
                       8, ENDPOINT_CDC, 0x80 | ENDPOINT_CDC, 64),
#endif
#if CONFIG_TINYUSB_MSC_ENABLED
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, USB_DESC_MSC, ENDPOINT_MSC,
                       0x80 | ENDPOINT_MSC, 64), // highspeed 512
#endif
#if CONFIG_TINYUSB_HID_ENABLED
    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, USB_DESC_HID, HID_PROTOCOL_NONE,
                       sizeof(desc_hid_report), 0x84, 16, 10),
#endif
#if CONFIG_TINYUSB_VENDOR_ENABLED
    // Interface number, string index, EP Out & IN address, EP size
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, USB_DESC_VENDOR, ENDPOINT_VENDOR,
                          0x80 | ENDPOINT_VENDOR, 64),
#endif
#if CONFIG_TINYUSB_MIDI_ENABLED
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, USB_DESC_MIDI, ENDPOINT_MIDI,
                        0x80 | ENDPOINT_MIDI,
                        (CFG_TUSB_RHPORT0_MODE & OPT_MODE_HIGH_SPEED) ? 512 : 64),
#endif
};

static std::string s_str_descriptor[USB_DESC_MAX_COUNT] =
{
    "", // NOT USED - SPECIAL CASED IN tud_descriptor_string_cb
    "", // USB_DESC_MANUFACTURER
    "", // USB_DESC_PRODUCT
    "", // USB_DESC_SERIAL_NUMBER
    "", // USB_DESC_CDC
    "", // USB_DESC_MSC
    "", // USB_DESC_HID
    "", // USB_DESC_VENDOR
    ""  // USB_DESC_MIDI
};
static uint16_t _desc_str[32];

// =============================================================================
// Device descriptor functions
// =============================================================================
void configure_usb_descriptor(tusb_desc_device_t *desc, uint16_t bcdDevice)
{
    if (desc)
    {
        memcpy(&s_descriptor, desc, sizeof(tusb_desc_device_t));
    }
    else if (bcdDevice)
    {
        s_descriptor.bcdDevice = bcdDevice;
    }
}

void configure_usb_descriptor_str(esp_usb_descriptor_index_t index,
                                  const char *value)
{
    s_str_descriptor[index].assign(value);
    s_str_descriptor[index].resize(31);
    ESP_LOGV(TAG, "USB-DESC(%d) %s\n", index, value);
}

// =============================================================================
// TinyUSB CALLBACKS
// =============================================================================

extern "C"
{

// Invoked when received GET DEVICE DESCRIPTOR
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_descriptor;
}

// Invoked when received GET CONFIGURATION DESCRIPTOR
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // for multiple configurations
    return desc_configuration;
}

// Invoked when received GET STRING DESCRIPTOR request
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    uint8_t chr_count;

    if (index == 0)
    {
        _desc_str[1] = 0x09;
        _desc_str[2] = 0x04;
        chr_count = 1;
    }
    else if (index >= USB_DESC_MAX_COUNT)
    {
        // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
        // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors
        return NULL;
    }
    else
    {
        chr_count = s_str_descriptor[index].length();

        // Ensure the string will fit into the target buffer
        if (chr_count > (TU_ARRAY_SIZE(_desc_str) - 1))
        {
            chr_count = TU_ARRAY_SIZE(_desc_str) - 1;
        }

        // Convert the ASCII string into UTF-16
        for(size_t idx = 0; idx < chr_count; idx++)
        {
            _desc_str[idx + 1] = s_str_descriptor[index].at(idx);
        }
    }

    // first byte is len, second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8) | ((chr_count << 1) + 2);

    return _desc_str;
}

#if CONFIG_USB_HID_ENABLED

// HID Report Descriptor
static uint8_t const desc_hid_report[] =
{
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD),),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE),)
};

// Invoked when received GET HID REPORT DESCRIPTOR request
uint8_t const *tud_hid_descriptor_report_cb(void)
{
    return desc_hid_report;
}

#endif // CONFIG_USB_HID_ENABLED

} // extern "C"
