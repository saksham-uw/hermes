# Hermes

Hermes is a standalone pocket Git operations console for the Waveshare
**ESP32-S3-Touch-LCD-3.49**. Its first application workflow creates and merges
known pull-request routes for `UltraWording/ultracontract` without placing a
GitHub or Slack credential on the device.

## Current experience

- Exact quadrant-block Hermes boot mark with a three-second loading sequence.
- Five-item menu: Git, status, settings, Wi-Fi, and switch off.
- A dormant Git navigation shell for the planned `main <- branch`,
  `prod <- main`, and merge workflows. It performs no network actions yet.
- `BOOT` moves the highlight down one row. A tap anywhere activates that
  highlighted row; touch coordinates never pick a different item. Short `PWR`
  goes back one level, and held `PWR` runs the 1.5-second shutdown sequence.
- Cyberpunk live telemetry for battery, Wi-Fi signal, uptime, heap, PSRAM, and CPU.
- Seven display themes and persistent brightness stored in ESP32 NVS flash—no SD
  card is mounted or required.
- Three baked Wi-Fi routes with scanning, active-route marking, verified switching,
  and automatic rollback when a new route has no internet.
- Avatar Lab's actual Kirby definition, 28 expressions, and 23 animation presets,
  compiled with the official `@bible-strong/avatar-core` SDK. The antialiased body
  and eye channels are recolored live by all seven Hermes themes; SDK-rendered
  transitions, preset blink timing, drowsiness, sleep, and wake behavior run on
  the device.
- A four-state behavior system chooses among curated Avatar Lab routines:
  `idle` during normal interaction, `rest` after 30 seconds without input,
  `thinking` while Hermes navigates or searches, and explicit `busy` routines
  during Git feature work.

## Planned Git action service

The separate `@hermes/actions-service` package is prepared for the later
integration phase but is not compiled into or contacted by the current firmware.
It validates that its GitHub token belongs to
`saksham-uw`, creates only the two supported PR directions, requests
`Ultrassure` and `CAI-yw`, announces newly created PRs from saksham's Slack
user account, and records idempotency progress so device retries resume partial
operations safely. GitHub titles, bodies, merge commits, and Slack text never
mention Hermes.

```bash
npm install
npm run actions:test
npm run actions:build
npm run actions:start
```

See [packages/actions-service/README.md](packages/actions-service/README.md) for
the required GitHub/Slack configuration and [task.md](task.md) for the complete
product contract. Deploy the action service to AWS Lambda with
`npm run actions:deploy`. That writes a local firmware header with the Function
URL and device token; it is gitignored and must not be committed.

## Build and flash

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

See [firmware/README.md](firmware/README.md) for hardware and firmware details.

## Avatar pipeline

The checked-in `.avatar.json` is the runtime definition exported from Avatar Lab,
not a hand-drawn approximation. To resync it from an Avatar Lab checkout and
regenerate the compressed ESP32 frames:

```bash
npm install
npm run avatar:sync -- /path/to/bible-strong-avatar-lab/src/features/studio/defaultStudioDocument.json
npm run avatar:generate
```

`avatar:generate` validates and renders through `@bible-strong/avatar-core`,
supersamples each SVG frame at 8×, and emits theme-recolorable eight-bit coverage
channels plus direct SDK morphs for every state-routine handoff. See
[`assets/avatar/README.md`](assets/avatar/README.md) for provenance and licensing.

## Dormant legacy stack

The former Codex/Cursor remote remains in the repository for reference, but it is
not compiled into or contacted by the active firmware:

| Path | Former role |
| --- | --- |
| `packages/bridge` | AWS IoT to Codex/Cursor bridge |
| `packages/protocol` | Legacy MQTT message types |
| `packages/cursor-hooks` | Cursor observation hooks |
| `deploy/iot` | AWS IoT provisioning |
| `scripts/codex-lunch.sh` | Codex/tmux helper |
| `scripts/hermes-stt.py` | Voice transcription service |
| `firmware/main/hermes_mqtt.*` | Device-side AWS MQTT client |
| `firmware/main/hermes_audio.*` | Former prompt recording/upload path |
| `firmware/main/hermes_sd.*` | Former SD-backed storage |

Legacy Node commands are deliberately namespaced under `npm run legacy:*`; none are
part of the normal device workflow.
