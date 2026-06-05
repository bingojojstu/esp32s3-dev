# OpenClaw LAN Bridge

A tiny FastAPI service that lets an ESP32 device on the LAN talk to a local
OpenClaw gateway with voice. It does the two jobs the ESP32 can't:

1. **STT** — turns 16 kHz / 16-bit / mono PCM into Chinese text via
   [`openai-whisper`](https://github.com/openai/whisper) (default model: `small`).
2. **OpenClaw proxy** — forwards the text to OpenClaw's OpenAI-Responses
   compatible `/v1/responses` endpoint and streams the SSE reply back to the
   device verbatim, so the existing ESP32 SSE parser works unchanged.

## Endpoints

| Method | Path     | Body                          | Reply                                   |
|--------|----------|-------------------------------|-----------------------------------------|
| GET    | `/health`| —                             | JSON `{status, whisper_model, ...}`     |
| POST   | `/chat`  | `{"text": "..."}`             | SSE stream forwarded from OpenClaw      |
| POST   | `/voice` | raw PCM 16 kHz/16 bit/mono LE | SSE stream: first event `stt`, then OpenClaw stream |

## Install (Mac mini)

```bash
git clone git@github.com:bingojojstu/esp32s3-dev.git
cd esp32s3-dev/tools/openclaw-bridge

# Optional but recommended: a venv keeps deps out of system python
python3 -m venv .venv
source .venv/bin/activate

pip install -r requirements.txt
```

`openai-whisper` pulls in `torch`, which is a few hundred MB. First install
takes a couple of minutes; subsequent installs are fast.

## Run

Easiest:

```bash
OPENCLAW_TOKEN="<your bearer>" ./run.sh
```

Manually:

```bash
export OPENCLAW_TOKEN="<your bearer>"
export OPENCLAW_URL="http://127.0.0.1:18789/v1/responses"
uvicorn bridge:app --host 0.0.0.0 --port 8000
```

On first start whisper loads the `small` model (~466 MB). You'll see
`[bridge] whisper ready in 3.2s` once it's done — then the service is
ready to take requests.

## Smoke tests

```bash
# liveness
curl -s http://127.0.0.1:8000/health | jq

# text round-trip (mirrors Phase 1 ESP32 BOOT-click behavior)
curl -N -X POST http://127.0.0.1:8000/chat \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，介绍一下你自己。"}'

# voice round-trip with any audio file
ffmpeg -i sample.m4a -ar 16000 -ac 1 -f s16le pipe:1 \
  | curl -N -X POST http://127.0.0.1:8000/voice \
        -H 'Content-Type: application/octet-stream' \
        --data-binary @-
```

You should see the SSE stream print on stdout in real time:

```
event: stt
data: {"text": "你好，介绍一下你自己。"}

event: response.output_text.delta
data: {"delta": "你好"}
...
data: [DONE]
```

## Config (env vars)

| Var                     | Default                                  | Notes                          |
|-------------------------|------------------------------------------|--------------------------------|
| `OPENCLAW_URL`          | `http://127.0.0.1:18789/v1/responses`    | OpenClaw gateway endpoint      |
| `OPENCLAW_TOKEN`        | _(required)_                             | Bearer token                   |
| `OPENCLAW_AGENT`        | `main`                                   | `x-openclaw-agent-id`          |
| `OPENCLAW_MODEL`        | `deepseek/deepseek-v4-flash`             | `x-openclaw-model`             |
| `OPENCLAW_USER`         | `esp32-voice`                            | Session key (conversation memory) |
| `OPENCLAW_MAX_TOKENS`   | `300`                                    | Max output tokens              |
| `WHISPER_MODEL`         | `small`                                  | tiny / base / small / medium / large |
| `WHISPER_LANG`          | `zh`                                     | ISO-639-1 code                 |
