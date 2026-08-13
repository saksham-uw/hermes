#!/usr/bin/env bash
# Observe-only Cursor hook: POST activity to hermes-bridge loopback inbox.
# Always returns quickly with allow (does NOT gate IDE approvals).
set -euo pipefail

EVENT="${1:-update}"
INBOX_URL="${HERMES_INBOX_URL:-http://127.0.0.1:8741/event}"

INPUT="$(cat || true)"

# Build JSON payload with python for safe escaping.
PAYLOAD="$(EVENT="$EVENT" INPUT="$INPUT" python3 - <<'PY'
import json, os
event = os.environ.get("EVENT", "update")
raw = os.environ.get("INPUT", "")
try:
    data = json.loads(raw) if raw.strip() else {}
except Exception:
    data = {"raw": raw[:500]}

summary = ""
for key in ("command", "text", "message", "prompt", "output", "diff", "status"):
    val = data.get(key)
    if isinstance(val, str) and val.strip():
        summary = val[:400]
        break
if not summary:
    summary = json.dumps(data)[:400] if data else event

state = "running"
if event in ("sessionEnd", "stop"):
    state = "idle"
elif event == "afterAgentResponse":
    state = "idle"
elif event == "sessionStart":
    state = "running"

print(json.dumps({
    "source": "cursor-hooks",
    "event": event,
    "summary": summary,
    "state": state,
}))
PY
)"

curl -fsS -m 2 -X POST "$INBOX_URL" \
  -H 'content-type: application/json' \
  -d "$PAYLOAD" >/dev/null 2>&1 || true

# Always allow — observe only. Do not block Cursor IDE.
case "$EVENT" in
  beforeShellExecution|beforeMCPExecution|preToolUse)
    printf '%s\n' '{"permission":"allow","continue":true}'
    ;;
  *)
    printf '%s\n' '{}'
    ;;
esac
