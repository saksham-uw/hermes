# Hermes Firmware — ESP32-S3-Touch-LCD-3.49

AWS IoT Core MQTT client + LVGL UI for Waveshare **ESP32-S3-Touch-LCD-3.49** (172×640, AXS15231B).

## Prerequisites

1. [ESP-IDF 5.3+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
2. Device certs from `npm run provision:iot` (written to `firmware/certs/`)
3. Waveshare board BSP / LCD example components (QSPI AXS15231B + touch). Copy or set `EXTRA_COMPONENT_DIRS` to Waveshare’s SDK when available.

## Configure

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig   # Hermes Configuration: Wi-Fi SSID/password, IoT endpoint
```

Or edit `sdkconfig.defaults` / set:

- `CONFIG_HERMES_WIFI_SSID`
- `CONFIG_HERMES_WIFI_PASSWORD`
- `CONFIG_HERMES_IOT_ENDPOINT` (default `acp99wijypwjp-ats.iot.us-east-1.amazonaws.com`)
- `CONFIG_HERMES_USER_ID` (`saksham`)
- `CONFIG_HERMES_THING_NAME` (`hermes-device`)

Embed certs: place PEMs in `certs/` (gitignored except `.gitkeep`):

- `device.cert.pem`
- `device.private.key`
- `AmazonRootCA1.pem`

## Build & flash

```bash
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## UI

- **Status** — agent state + summary
- **Approval** — Approve / Deny (long-press Approve for shell)
- **Prompt** — preset buttons + short text entry

BOOT button cycles screens when touch/LCD BSP is not linked (`CONFIG_HERMES_UI_CONSOLE=y` fallback).

## Topics

- Publish: `hermes/<user>/device/up`
- Subscribe: `hermes/<user>/device/down`
