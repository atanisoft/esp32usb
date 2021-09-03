idf_component_register(REQUIRES esp_rom app_update heap spi_flash freertos soc driver)

idf_component_get_property(FREERTOS_ORIG_INCLUDE_PATH freertos ORIG_INCLUDE_PATH)

target_compile_options(${COMPONENT_TARGET} INTERFACE
    "-DCFG_TUSB_MCU=OPT_MCU_ESP32S2"
)

target_include_directories(${COMPONENT_TARGET} INTERFACE
    "${FREERTOS_ORIG_INCLUDE_PATH}"
    "${COMPONENT_DIR}/include/"
    "${COMPONENT_DIR}/src/tinyusb/hw/bsp/"
    "${COMPONENT_DIR}/src/tinyusb/src/"
    "${COMPONENT_DIR}/src/tinyusb/src/device"
)

target_sources(${COMPONENT_TARGET} INTERFACE
    "${COMPONENT_DIR}/src/tinyusb/src/tusb.c"
    "${COMPONENT_DIR}/src/tinyusb/src/common/tusb_fifo.c"
    "${COMPONENT_DIR}/src/tinyusb/src/device/usbd.c"
    "${COMPONENT_DIR}/src/tinyusb/src/device/usbd_control.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/cdc/cdc_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/dfu/dfu_rt_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/hid/hid_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/midi/midi_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/msc/msc_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/net/net_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/usbtmc/usbtmc_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/class/vendor/vendor_device.c"
    "${COMPONENT_DIR}/src/tinyusb/src/portable/espressif/esp32sx/dcd_esp32sx.c"
    "${COMPONENT_DIR}/src/usb.cpp"
    "${COMPONENT_DIR}/src/usb_cdc.cpp"
    "${COMPONENT_DIR}/src/usb_msc.cpp"
)
