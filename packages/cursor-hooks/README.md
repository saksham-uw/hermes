# Hermes Cursor observe hooks

These hooks **mirror** Cursor IDE side-panel activity to the Hermes bridge.
They do **not** approve/deny tool calls from the ESP32 (Cursor hook `ask` is unreliable, and hook timeouts cannot cover a lunch break).

## Install into a project on EC2

```bash
# From your project root (the repo you open in Cursor Remote-SSH):
mkdir -p .cursor
cp -R /path/to/hermes/packages/cursor-hooks/hooks.json .cursor/hooks.json
mkdir -p .cursor/scripts
cp /path/to/hermes/packages/cursor-hooks/scripts/notify.sh .cursor/scripts/notify.sh
chmod +x .cursor/scripts/notify.sh
```

Update `.cursor/hooks.json` command paths if needed:

```json
"command": ".cursor/scripts/notify.sh sessionStart"
```

Ensure `hermes-bridge` is running so `http://127.0.0.1:8741/event` accepts events.

Optional:

```bash
export HERMES_INBOX_URL=http://127.0.0.1:8741/event
```
