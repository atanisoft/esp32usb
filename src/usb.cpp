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

// if TinyUSB debug is enabled set the local log level higher than any of the
// pre-defined log levels.
#if CONFIG_TINYUSB_DEBUG
#define LOG_LOCAL_LEVEL 0xFF
#endif

#include <driver/gpio.h>
#include <driver/periph_ctrl.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_task.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4,3,0)
#include <esp32s2/rom/usb/chip_usb_dw_wrapper.h>
#include <esp32s2/rom/usb/usb_persist.h>
#else
#ifndef USBDC_PERSIST_ENA
#define USBDC_PERSIST_ENA (1<<31)
#endif
#endif // IDF v4.3+
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/usb_hal.h>
#include <soc/gpio_periph.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_periph.h>
#include <soc/usb_wrap_struct.h>
#include <string>
#include "usb.h"

static constexpr const char * const TAG = "USB";

#if CONFIG_TINYUSB_CDC_ENABLED
void init_usb_cdc();
#endif

void init_usb_subsystem(bool external_phy)
{
    ESP_LOGI(TAG, "Initializing USB peripheral");
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4,3,0)
    if ((chip_usb_get_persist_flags() & USBDC_PERSIST_ENA) == USBDC_PERSIST_ENA)
#else
    if (USB_WRAP.date.val == USBDC_PERSIST_ENA)
    {
        // Enable USB/IO_MUX peripheral reset on next reboot.
        REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE);
        REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE);
    }
    else
    {
        // Normal startup flow, reinitailize the USB peripheral.
        periph_module_reset(PERIPH_USB_MODULE);
        periph_module_enable(PERIPH_USB_MODULE);
    }
#endif // IDF v4.3+

    usb_hal_context_t hal;
    hal.use_external_phy = external_phy;
    ESP_LOGD(TAG, "Initializing USB HAL");
    usb_hal_init(&hal);

    if (external_phy)
    {
        gpio_output_set_high(0x10, 0, 0x1E, 0xE);
    }
    else
    {
        ESP_LOGV(TAG, "Setting GPIO %d drive to %d", USBPHY_DM_NUM
               , GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)USBPHY_DM_NUM
                                , (gpio_drive_cap_t)GPIO_DRIVE_CAP_3);
        ESP_LOGV(TAG, "Setting GPIO %d drive to %d", USBPHY_DP_NUM
               , GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)USBPHY_DP_NUM
                                , (gpio_drive_cap_t)GPIO_DRIVE_CAP_3);
    }

    for (const usb_iopin_dsc_t* iopin = usb_periph_iopins; iopin->pin != -1;
         ++iopin)
    {
        if (external_phy || (iopin->ext_phy_only == 0))
        {
            gpio_pad_select_gpio(iopin->pin);
            if (iopin->is_output)
            {
                ESP_LOGV(TAG, "Configuring USB GPIO %d as OUTPUT", iopin->pin);
                gpio_matrix_out(iopin->pin, iopin->func, false, false);
            }
            else
            {
                ESP_LOGV(TAG, "Configuring USB GPIO %d as INPUT", iopin->pin);
                gpio_matrix_in(iopin->pin, iopin->func, false);
                gpio_pad_input_enable(iopin->pin);
            }
            gpio_pad_unhold(iopin->pin);
        }
    }

#if CONFIG_TINYUSB_CDC_ENABLED
    init_usb_cdc();
#endif

    ESP_LOGI(TAG, "USB system initialized");
}

static void usb_device_task(void *param)
{
    ESP_LOGI(TAG, "Initializing TinyUSB");
    ESP_ERROR_CHECK(tusb_init());

    ESP_LOGI(TAG, "TinyUSB Task (%s) starting execution",
             CONFIG_TINYUSB_TASK_NAME);
    while (1)
    {
        tud_task();
    }
}

// sanity check that the user did not define the task priority too low.
static_assert(CONFIG_TINYUSB_TASK_PRIORITY > ESP_TASK_MAIN_PRIO,
              "TinyUSB task must have a higher priority than the app_main task.");

void start_usb_task()
{
    if (xTaskCreate(usb_device_task, CONFIG_TINYUSB_TASK_NAME,
                    CONFIG_TINYUSB_TASK_STACK_SIZE, nullptr,
                    CONFIG_TINYUSB_TASK_PRIORITY, nullptr) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task for USB.");
        abort();
    }
    ESP_LOGI(TAG, "Created TinyUSB task: %s", CONFIG_TINYUSB_TASK_NAME);
}

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

// Used to generate the USB PID based on enabled interfaces.
#define _PID_MAP(itf, n)  ((CFG_TUD_##itf) << (n))

/// USB Device Descriptor.
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
                           _PID_MAP(VENDOR, 4) | _PID_MAP(DFU_RT, 5)),
    .bcdDevice          = CONFIG_TINYUSB_DESC_BCDDEVICE,
    .iManufacturer      = USB_DESC_MANUFACTURER,
    .iProduct           = USB_DESC_PRODUCT,
    .iSerialNumber      = USB_DESC_SERIAL_NUMBER,
    .bNumConfigurations = 0x01
};

/// USB Device Endpoint assignments.
///
/// NOTE: The ESP32-S2 has four input FIFOs available, unfortunately this will
/// result in some overlap between features. The notification endpoint is not
/// connected to the FIFOs.
///
/// @todo switch to dynamic endpoint assignment except for CDC and NOTIF which
/// require static definitions.
typedef enum
{
    /// Vendor endpoint.
    ENDPOINT_VENDOR_OUT = 0x01,

    /// Mass Storage endpoint.
    ENDPOINT_MSC_OUT = 0x02,

    /// CDC endpoint.
    ///
    /// NOTE: This matches the ESP32-S2 ROM code mapping.
    ENDPOINT_CDC_OUT = 0x03,

    /// MIDI endpoint.
    ENDPOINT_MIDI_OUT = 0x04,

    /// HID endpoint.
    ENDPOINT_HID_IN = 0x81,

    /// Mass Storage endpoint.
    ENDPOINT_MSC_IN = 0x82,

    /// Vendor endpoint.
    ENDPOINT_VENDOR_IN = 0x83,

    /// MIDI endpoint.
    ENDPOINT_MIDI_IN = 0x83,

    /// CDC endpoint.
    ///
    /// NOTE: This matches the ESP32-S2 ROM code mapping.
    ENDPOINT_CDC_IN = 0x84,

    /// Notification endpoint.
    ///
    /// NOTE: This matches the ESP32-S2 ROM code mapping.
    ENDPOINT_NOTIF = 0x85,
} esp_usb_endpoint_t;

/// USB Interface indexes.
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
#if CONFIG_TINYUSB_DFU_ENABLED 
    ITF_NUM_DFU_RT,
#endif
    ITF_NUM_TOTAL
} esp_usb_interface_t;

/// Total size of the USB device descriptor configuration data.
static constexpr uint16_t USB_DESCRIPTORS_CONFIG_TOTAL_LEN =
    TUD_CONFIG_DESC_LEN +
    (CONFIG_TINYUSB_CDC_ENABLED * TUD_CDC_DESC_LEN) +
    (CONFIG_TINYUSB_MSC_ENABLED * TUD_MSC_DESC_LEN) +
    (CONFIG_TINYUSB_HID_ENABLED * TUD_HID_DESC_LEN) +
    (CONFIG_TINYUSB_VENDOR_ENABLED * TUD_VENDOR_DESC_LEN) +
    (CONFIG_TINYUSB_MIDI_ENABLED * TUD_MIDI_DESC_LEN) +
    (CONFIG_TINYUSB_DFU_ENABLED * TUD_DFU_RT_DESC_LEN);

#if CONFIG_TINYUSB_CDC_ENABLED
static_assert(CONFIG_TINYUSB_CDC_FIFO_SIZE == 64, "CDC FIFO size must be 64");
#endif
#if CONFIG_TINYUSB_MSC_ENABLED
static_assert(CONFIG_TINYUSB_MSC_FIFO_SIZE == 64, "MSC FIFO size must be 64");
#endif
#if CONFIG_TINYUSB_VENDOR_ENABLED
static_assert(CONFIG_TINYUSB_VENDOR_FIFO_SIZE == 64
            , "Vendor FIFO size must be 64");
#endif
#if CONFIG_TINYUSB_MIDI_ENABLED
static_assert(CONFIG_TINYUSB_MIDI_FIFO_SIZE == 64
            , "MIDI FIFO size must be 64");
#endif

/// USB device descriptor configuration data.
uint8_t const desc_configuration[USB_DESCRIPTORS_CONFIG_TOTAL_LEN] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0,
                          USB_DESCRIPTORS_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          CONFIG_TINYUSB_MAX_POWER_USAGE),
#if CONFIG_TINYUSB_CDC_ENABLED
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, USB_DESC_CDC, ENDPOINT_NOTIF, 8,
                       ENDPOINT_CDC_OUT, ENDPOINT_CDC_IN,
                       CONFIG_TINYUSB_CDC_FIFO_SIZE),
#endif
#if CONFIG_TINYUSB_MSC_ENABLED
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, USB_DESC_MSC, ENDPOINT_MSC_OUT,
                       ENDPOINT_MSC_IN, CONFIG_TINYUSB_MSC_FIFO_SIZE),
#endif
#if CONFIG_TINYUSB_HID_ENABLED
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, USB_DESC_HID, HID_PROTOCOL_NONE,
                       sizeof(desc_hid_report), ENDPOINT_HID_IN,
                       CONFIG_TINYUSB_HID_BUFSIZE, 10),
#endif
#if CONFIG_TINYUSB_VENDOR_ENABLED
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, USB_DESC_VENDOR, ENDPOINT_VENDOR_OUT,
                          ENDPOINT_VENDOR_IN, CONFIG_TINYUSB_VENDOR_FIFO_SIZE),
#endif
#if CONFIG_TINYUSB_MIDI_ENABLED
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, USB_DESC_MIDI, ENDPOINT_MIDI_OUT,
                        ENDPOINT_MIDI_IN, CONFIG_TINYUSB_MIDI_FIFO_SIZE),
#endif
#if CONFIG_TINYUSB_DFU_ENABLED
    TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, USB_DESC_DFU, 0x0d,
                          CONFIG_TINYUSB_DFU_DISCONNECT_DELAY,
                          CONFIG_TINYUSB_DFU_BUFSIZE),
#endif
};

/// USB device descriptor strings.
///
/// NOTE: Only ASCII characters are supported at this time.
static std::string s_str_descriptor[USB_DESC_MAX_COUNT] =
{
    "",     // LANGUAGE (unused in tud_descriptor_string_cb)
    "",     // USB_DESC_MANUFACTURER
    "",     // USB_DESC_PRODUCT
    "",     // USB_DESC_SERIAL_NUMBER
    "",     // USB_DESC_CDC
    "",     // USB_DESC_MSC
    "",     // USB_DESC_HID
    "",     // USB_DESC_VENDOR
    "",     // USB_DESC_MIDI
    "",     // USB_DESC_DFU
};

/// Maximum length of the USB device descriptor strings.
static constexpr size_t MAX_DESCRIPTOR_LEN = 126;

/// Temporary holding buffer for USB device descriptor string data in UTF-16
/// format.
///
/// NOTE: Only ASCII characters are supported at this time.
static uint16_t _desc_str[MAX_DESCRIPTOR_LEN + 1];

// =============================================================================
// Device descriptor functions
// =============================================================================
void configure_usb_descriptor(tusb_desc_device_t *desc, uint16_t version)
{
    if (desc)
    {
        memcpy(&s_descriptor, desc, sizeof(tusb_desc_device_t));
    }
    else if (version)
    {
        s_descriptor.bcdDevice = version;
    }
}

void configure_usb_descriptor_str(esp_usb_descriptor_index_t index,
                                  const char *value)
{
    // truncate the descriptor string (if needed).
    s_str_descriptor[index].assign(value, MAX_DESCRIPTOR_LEN);
    ESP_LOGI(TAG, "Setting USB descriptor %d text to: %s", index, value);
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
    // clear the last descriptor
    bzero(_desc_str, TU_ARRAY_SIZE(_desc_str));

    if (index == 0)
    {
        _desc_str[1] = tu_htole16(0x0409);
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
        // TODO: evaluate if std::copy can be used here instead.
        // copy the string into the temporary array starting at offset 1
        size_t idx = 1;
        for (char ch : s_str_descriptor[index])
        {
            _desc_str[idx++] = tu_htole16(ch);
        }
        chr_count = s_str_descriptor[index].length();
    }

    // length and type
    _desc_str[0] = tu_htole16((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

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


#if CONFIG_USB_DFU_ENABLED
// Invoked when the DFU Runtime mode is requested
void tud_dfu_rt_reboot_to_dfu(void)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_PROCPU_RST);
}
#endif // CONFIG_USB_DFU_ENABLED

} // extern "C"
