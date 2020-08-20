# What is esp32s2usb?

esp32s2usb is an ESP-IDF component that provides a replacement for the ESP-IDF TinyUSB component. This code tracks https://github.com/hathach/tinyusb latest code rather than https://github.com/espressif/tinyusb.

# How to use?
In your project, add this as a submodule to your `components/` directory.

```
git submodule add https://github.com/atanisoft/esp32s2usb.git
git submodule update --recursive --init -- esp32s2usb
```

The library can be configured via `idf.py menuconfig` under `TinyUSB (esp32s2usb)`.

# Integrating esp32s2usb with your project

In the `app_main()` method you should have code similar to the following:

```
void app_main() {
  init_usb_subsystem();
  configure_usb_descriptor_str(USB_DESC_MANUFACTURER, "esp32s2usb");
  configure_usb_descriptor_str(USB_DESC_PRODUCT, "esp32s2usb Device");
  configure_usb_descriptor_str(USB_DESC_SERIAL_NUMBER, "1234567890");
  start_usb_task();
  .... rest of application code
}
```

## Integrating a virtual disk drive
If you are configuring a virtual disk you will need to configure it prior to calling `start_usb_task()`:

```
static const char * const readme_txt =
  "This is esp32s2usb's MassStorage Class demo.\r\n\r\n"
  "If you find any bugs or get any questions, feel free to file an\r\n"
  "issue at github.com/atanisoft/esp32s2usb"

void app_main() {
  init_usb_subsystem();
  configure_usb_descriptor_str(USB_DESC_MANUFACTURER, "esp32s2usb");
  configure_usb_descriptor_str(USB_DESC_PRODUCT, "esp32s2usb Device");
  configure_usb_descriptor_str(USB_DESC_SERIAL_NUMBER, "1234567890");
  configure_virtual_disk("esp32s2usb", 0x0100);
  add_readonly_file_to_virtual_disk("readme.txt", readme_txt, strlen(readme_txt));
  start_usb_task();
```

### Virtual Disk limitations

1. The virtual disk support is currently limited to around 2MiB in size but may be configurable in the future. 
2. The currently running firmware is not exposed via the virtual disk interface but will be optionally exposed via a method call in the future.
