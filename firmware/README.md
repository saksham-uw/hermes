# Hermes firmware

Standalone ESP-IDF firmware for the Waveshare **ESP32-S3-Touch-LCD-3.49**
(172×640 AXS15231B panel, presented as a 640×172 landscape interface).

## Prerequisites

1. ESP-IDF 5.3 or newer.
2. A supported ESP32-S3-Touch-LCD-3.49 board with PSRAM.
3. One of the three Wi-Fi networks configured in `main/wifi_connect.c`, if network
   telemetry, route switching, or Git workflows are needed.

AWS credentials, IoT certificates, an SD card, the EC2 bridge, Codex, Cursor, and
the audio codecs are not required by this firmware.

## Build and flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Controls

| Input | Behavior |
| --- | --- |
| `BOOT` press | Move the highlight to the next item, wrapping at the end. Never activates. |
| Screen tap | Activate the currently highlighted item. Touch coordinates must not select a different row. |
| Short `PWR` | Go back one navigation level |
| Hold `PWR` | Play the 1.5-second shutdown sequence and power down |
| `RESET` | Hardware reset; it is not an application input |

Touch swipes do not navigate. This intentionally keeps traversal one-directional.
BOOT and PWR edges are captured by GPIO interrupts, so short presses are retained
even while the display is flushing an animation frame.

## Persistence and networking

Theme, brightness, and the selected Wi-Fi route are stored in the ESP32 NVS
partition. Display settings no longer touch the TF/SD interface. Wi-Fi scanning and
connection attempts run in background FreeRTOS tasks so a loading indicator can
continue animating.

GitHub and Slack credentials are held by the AWS Lambda action service. The
firmware stores only the Function URL and a device token, generated locally by
`npm run actions:deploy`.

The three requested credentials are compiled into `main/wifi_connect.c`. Treat a
firmware image as sensitive because those credentials can be extracted from it.

## Active modules

- `hermes_ui` — screens, controls, loading and power state machine.
- `hermes_avatar` — the on-device Avatar Core timeline, blink/lifecycle policy,
  compressed-frame decoder, and theme-aware RGB565 compositor.
- `hermes_avatar_assets` — generated Avatar Lab geometry, SDK transition frames,
  expressions, and semantic animation metadata. Regenerate it from the repository
  root with `npm run avatar:generate`.
- `hermes_gfx` / `display_axs15231b` — framebuffer rendering and panel transport.
- `hermes_settings` — NVS-backed theme and brightness.
- `wifi_connect` — baked route discovery, selection and rollback.

The old MQTT, audio and SD modules are retained as dormant source files but omitted
from `main/CMakeLists.txt`.

The custom partition table gives the factory application 12 MiB so the high-quality
Avatar Lab frame set fits without reducing its antialiasing or transition cadence.
