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

#include <driver/periph_ctrl.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#if CONFIG_IDF_TARGET_ESP32S2
#include <esp32s2/rom/usb/chip_usb_dw_wrapper.h>
#include <esp32s2/rom/usb/usb_persist.h>
#elif CONFIG_IDF_TARGET_ESP32S3
#include <esp32s3/rom/usb/chip_usb_dw_wrapper.h>
#include <esp32s3/rom/usb/usb_persist.h>
#else
#error Unsupported architecture.
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/gpio_periph.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_periph.h>
#include "usb.h"

/// Tag used for all logging.
static constexpr const char * const TAG = "USB:CDC";

#if CONFIG_ESPUSB_CDC

/// Current state of the USB CDC interface.
static esp_line_state_t cdc_line_state = LINE_STATE_DISCONNECTED;

/// Maximum number of ticks to allow for TX to complete before giving up.
static constexpr TickType_t WRITE_TIMEOUT_TICKS =
    pdMS_TO_TICKS(CONFIG_ESPUSB_CDC_WRITE_FLUSH_TIMEOUT);

/// System shutdown hook used for flagging that the restart should go into a
/// download mode rather than normal startup mode.
///
/// NOTE: This will disable the USB peripheral restart on startup and will
/// require manual reset on reinitialization.
static void IRAM_ATTR usb_shutdown_hook(void)
{
    // Check if it there is a request to restart into download mode.
    if (cdc_line_state == LINE_STATE_REQUEST_DOWNLOAD ||
        cdc_line_state == LINE_STATE_REQUEST_DOWNLOAD_DFU)
    {
        ESP_EARLY_LOGV(TAG, "Disabling USB peripheral restart on next boot");
        REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE);
        REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE);

        periph_module_disable(PERIPH_TIMG1_MODULE);
        if (cdc_line_state == LINE_STATE_REQUEST_DOWNLOAD)
        {
            chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
        }
        else
        {
            chip_usb_set_persist_flags(USBDC_BOOT_DFU);
            periph_module_disable(PERIPH_TIMG0_MODULE);
        }

        ESP_EARLY_LOGV(TAG, "Setting next boot mode to download");
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_PROCPU_RST);
    }
}

/// Initializes the USB CDC.
void init_usb_cdc()
{
    // register shutdown hook for rebooting into download mode
    ESP_ERROR_CHECK(esp_register_shutdown_handler(usb_shutdown_hook));
}

// Attempts to write a buffer to the USB CDC if a device is present.
size_t write_to_cdc(const char *buf, size_t size)
{
    size_t offs = 0;
    uint32_t ticks_start = xTaskGetTickCount();
    uint32_t ticks_now = ticks_start;
    if (cdc_line_state != LINE_STATE_CONNECTED &&
        cdc_line_state != LINE_STATE_MAYBE_CONNECTED)
    {
        goto exit_write_to_cdc;
    }

    // while there is still data remaining and we have not timed out keep
    // trying to send data
    while (offs < size)
    {
        // track the current time
        ticks_now = xTaskGetTickCount();
        if ((ticks_now - WRITE_TIMEOUT_TICKS) > ticks_start)
        {
            break;
        }

        // pick the smallest buffer size that we can push to the CDC
        uint32_t to_send = std::min(tud_cdc_write_available(), size - offs);

        // attempt to send the full buffer in one shot, this will send only
        // up to the point of filling the FIFO and return the amount sent.
        uint16_t sent = tud_cdc_write(buf + offs, to_send);

        // if we successfully queued at least one character in the CDC TX
        // buffer, flush it out onto the wire.
        if (sent > 0)
        {
            tud_cdc_write_flush();
        }
        offs += sent;
    }

    // If we still have some data left to transmit by the time we reach
    // here a FIFO overflow occurred.
    if (size)
    {
        ESP_LOGE(TAG, "TX FIFO Overflow! %d remaining after timeout.", size);
    }

exit_write_to_cdc:
    return offs;
}

// Default implementation of usb_line_state_changed_cb which allows restart.
TU_ATTR_WEAK bool usb_line_state_changed_cb(esp_line_state_t state, bool download)
{
    if (download)
    {
        ESP_LOGI(TAG, "Firmware download request received, allowing restart");
    }
    return true;
}

// Override the flag for line state such that it will trigger DFU mode on the
// next restart.
void request_dfu_mode()
{
    cdc_line_state = LINE_STATE_REQUEST_DOWNLOAD_DFU;
}

extern "C"
{

// =============================================================================
// TinyUSB CALLBACKS
// =============================================================================

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    ESP_LOGV(TAG, "tud_cdc_line_state_cb(%d, %d, %d), state: %d", itf, dtr,
             rts, cdc_line_state);
    if (!dtr && rts)
    {
        if (cdc_line_state == LINE_STATE_DISCONNECTED ||
            cdc_line_state == LINE_STATE_CONNECTED)
        {
            ESP_LOGD(TAG, "Possible esptool request, waiting for reconnect");
            cdc_line_state = LINE_STATE_MAYBE_ENTER_DOWNLOAD_DTR;
        }
        else
        {
            ESP_LOGI(TAG, "USB device disconnected");
            cdc_line_state = LINE_STATE_DISCONNECTED;
        }
    }
    else if (dtr && rts)
    {
        if (cdc_line_state == LINE_STATE_MAYBE_ENTER_DOWNLOAD_DTR)
        {
            ESP_LOGD(TAG, "Possible esptool request, waiting for rts low");
            cdc_line_state = LINE_STATE_MAYBE_CONNECTED;
        }
        else
        {
            ESP_LOGI(TAG, "USB device connected");
            cdc_line_state = LINE_STATE_CONNECTED;
        }
    }
    else if (dtr && !rts)
    {
        if (cdc_line_state == LINE_STATE_MAYBE_CONNECTED)
        {
            ESP_LOGD(TAG, "Possible esptool request, waiting for disconnect");
            cdc_line_state = LINE_STATE_MAYBE_ENTER_DOWNLOAD_RTS;
        }
        else
        {
            ESP_LOGI(TAG, "USB device disconnected");
            cdc_line_state = LINE_STATE_DISCONNECTED;
        }
    }
    else if (!dtr && !rts)
    {
        if (cdc_line_state == LINE_STATE_MAYBE_ENTER_DOWNLOAD_RTS)
        {
            ESP_LOGD(TAG, "esptool firmware upload requested");
            // request to restart in download mode
            cdc_line_state = LINE_STATE_REQUEST_DOWNLOAD;
        }
        else
        {
            ESP_LOGI(TAG, "USB device disconnected");
            cdc_line_state = LINE_STATE_DISCONNECTED;
        }
    }
    // check if the callback will handle the restart when there is a download
    // request pending.
    bool download = (cdc_line_state == LINE_STATE_REQUEST_DOWNLOAD ||
                     cdc_line_state == LINE_STATE_REQUEST_DOWNLOAD_DFU);
    bool restart = usb_line_state_changed_cb(cdc_line_state, download);

    // restart the system if the callback is not going to handle it and there
    // is a pending download request.
    if (restart && download)
    {
        ESP_LOGV(TAG, "Restarting...");
        esp_restart();
    }
}

} // extern "C"

#endif // CONFIG_USBUSB_CDC