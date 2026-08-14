#!/usr/bin/env bash
# One-time + start local Whisper STT on the EC2 box (no OpenAI key).
set -euo pipefail

ROOT="${HERMES_ROOT:-$HOME/hermes}"
VENV="${HERMES_STT_VENV:-$HOME/.hermes/stt-venv}"
PORT="${HERMES_STT_PORT:-8765}"
MODEL="${HERMES_STT_MODEL:-tiny.en}"

mkdir -p "$(dirname "$VENV")"
sudo apt-get update -y
sudo apt-get install -y python3-venv python3-pip ffmpeg

if [[ ! -x "$VENV/bin/python" ]]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install -U pip faster-whisper

echo
echo "Starting STT on 127.0.0.1:${PORT} model=${MODEL}"
echo "Leave this running. In another tmux pane: npm run start:bridge"
echo
export HERMES_STT_PORT="$PORT"
export HERMES_STT_MODEL="$MODEL"
exec "$VENV/bin/python" "$ROOT/scripts/hermes-stt.py"
