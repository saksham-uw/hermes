# Hermes

Pocket remote for **Codex** (approve / deny / prompt) and **Cursor IDE observe**, over **AWS IoT Core**.

Designed for a private EC2 box with **no public SSH**: both the EC2 bridge and the Waveshare ESP32-S3-Touch-LCD-3.49 dial **out** to IoT Core.

## Architecture

```
ESP32  --mTLS MQTT-->  AWS IoT Core  <--SigV4 MQTT--  hermes-bridge on EC2
                                                      |-- Codex app-server (loopback)
                                                      |-- Cursor hooks inbox :8741
```

Topics:

- `hermes/saksham/device/up` — device → bridge
- `hermes/saksham/device/down` — bridge → device

## Quick start

### 1. Install JS packages

```bash
cd "/Users/saksham/Weekend Projects/hermes"
npm install
npm run build
```

### 2. Provision AWS IoT Core

```bash
chmod +x deploy/iot/provision.sh scripts/codex-lunch.sh packages/cursor-hooks/scripts/notify.sh
npm run provision:iot
# Optional: attach bridge IAM policy to your EC2 instance role
HERMES_EC2_INSTANCE_PROFILE_ROLE=YourEc2Role npm run provision:iot
```

This creates Thing `hermes-device`, device certs under `firmware/certs/`, IoT policy, IAM policy `HermesBridgeIoT`, and writes `.env`.

### 3. Run bridge (laptop smoke test or EC2)

```bash
cp .env.example .env   # if needed; provision already writes .env
# Set HERMES_CODEX_SPAWN=false if Codex is not installed locally
export HERMES_CODEX_SPAWN=false
npm run start:bridge
```

On EC2 (lunch mode):

```bash
./scripts/codex-lunch.sh
# lock laptop — control from the ESP32
```

Systemd (optional):

```bash
sudo cp -R . /opt/hermes
sudo cp deploy/systemd/hermes-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now hermes-bridge
```

### 4. Cursor observe hooks

See [packages/cursor-hooks/README.md](packages/cursor-hooks/README.md). Copy hooks into the project you open via Cursor Remote-SSH.

### 5. Flash the ESP32

See [firmware/README.md](firmware/README.md). After `provision:iot`, set Wi-Fi in `idf.py menuconfig`, then `idf.py flash monitor`.

## Packages

| Path | Role |
| --- | --- |
| `packages/protocol` | Shared MQTT JSON types |
| `packages/bridge` | EC2 service: IoT + Codex + Cursor inbox |
| `packages/cursor-hooks` | Observe-only IDE hooks |
| `firmware` | ESP-IDF app for Waveshare 3.49 |
| `deploy/iot` | AWS IoT / IAM provisioning |
| `scripts/codex-lunch.sh` | tmux lunch session helper |

## Security notes

- Codex app-server listens on `127.0.0.1` only with a capability token in `~/.hermes/`.
- Device IoT policy is limited to `hermes/<user>/device/{up,down}`.
- Bridge uses IAM (`hermes-bridge-*` client ids).
- Cursor hooks never gate approvals (observe only).
