#!/usr/bin/env bash
# Helper: source ./run.sh OR run it with bash to start the bridge.
# Fill in OPENCLAW_TOKEN below or export it before running.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# --- Required ---------------------------------------------------------
export OPENCLAW_TOKEN="${OPENCLAW_TOKEN:-PUT_TOKEN_HERE}"

# --- Optional (defaults match Phase 1) --------------------------------
export OPENCLAW_URL="${OPENCLAW_URL:-http://127.0.0.1:18789/v1/responses}"
export OPENCLAW_AGENT="${OPENCLAW_AGENT:-main}"
export OPENCLAW_MODEL="${OPENCLAW_MODEL:-deepseek/deepseek-v4-flash}"
export OPENCLAW_USER="${OPENCLAW_USER:-esp32-voice}"
export OPENCLAW_MAX_TOKENS="${OPENCLAW_MAX_TOKENS:-300}"
export WHISPER_MODEL="${WHISPER_MODEL:-small}"
export WHISPER_LANG="${WHISPER_LANG:-zh}"

if [[ "${OPENCLAW_TOKEN}" == "PUT_TOKEN_HERE" ]]; then
    echo "ERROR: set OPENCLAW_TOKEN before running this script" >&2
    exit 1
fi

# Bind to 0.0.0.0 so the ESP32 on the LAN can reach us.
exec uvicorn bridge:app --host 0.0.0.0 --port 8000 --log-level info
