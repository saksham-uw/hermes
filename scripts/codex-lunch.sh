#!/usr/bin/env bash
# Start a lunch-proof Codex + Hermes session on EC2 (tmux).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SESSION="${HERMES_TMUX_SESSION:-hermes-codex}"
CWD="${HERMES_CODEX_CWD:-$PWD}"

if [[ -f "$ROOT/.env" ]]; then
  # shellcheck disable=SC1091
  set -a && source "$ROOT/.env" && set +a
fi

export HERMES_CODEX_CWD="${HERMES_CODEX_CWD:-$CWD}"
export HERMES_CODEX_SPAWN="${HERMES_CODEX_SPAWN:-true}"
export HERMES_CODEX_WS_URL="${HERMES_CODEX_WS_URL:-ws://127.0.0.1:4520}"

if ! command -v tmux >/dev/null 2>&1; then
  echo "tmux is required. Install tmux and retry." >&2
  exit 1
fi

if ! command -v codex >/dev/null 2>&1; then
  echo "codex CLI not found on PATH. Install Codex CLI on this host." >&2
  exit 1
fi

if ! command -v node >/dev/null 2>&1; then
  echo "node is required." >&2
  exit 1
fi

cd "$ROOT"
if [[ ! -d node_modules ]]; then
  npm install
fi
npm run build

BRIDGE_MODE="tmux"
if systemctl is-enabled hermes-bridge.service >/dev/null 2>&1; then
  sudo systemctl restart hermes-bridge.service || true
  BRIDGE_MODE="systemd"
fi

if tmux has-session -t "$SESSION" 2>/dev/null; then
  echo "tmux session '$SESSION' already exists. Attach with: tmux attach -t $SESSION"
else
  tmux new-session -d -s "$SESSION" -c "$HERMES_CODEX_CWD"
  tmux rename-window -t "$SESSION:0" codex
  if [[ "$BRIDGE_MODE" == "tmux" ]]; then
    tmux new-window -t "$SESSION" -n bridge -c "$ROOT"
    tmux send-keys -t "$SESSION:bridge" "npm run start:bridge" C-m
  fi
  tmux send-keys -t "$SESSION:codex" "echo 'Codex app-server is managed by hermes-bridge (HERMES_CODEX_SPAWN=true).'" C-m
fi

cat <<EOF

Hermes lunch session ready.

  tmux:     tmux attach -t $SESSION
  bridge:   $BRIDGE_MODE
  cwd:      $HERMES_CODEX_CWD
  mqtt:     AWS IoT Core (${HERMES_IOT_ENDPOINT:-set HERMES_IOT_ENDPOINT})

Safe to lock your laptop. Control Codex from the ESP32 (approve / deny / prompt).

EOF
