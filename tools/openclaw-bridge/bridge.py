"""
OpenClaw LAN voice bridge for ESP32.

Endpoints:
  POST /health -> liveness + which whisper model is loaded.

  POST /chat   -> body: {"text": "..."}
                  Forwards text directly to OpenClaw, streams the SSE reply
                  back. Useful for the existing BOOT-click "death letter"
                  diagnostic path (ESP32 stays on the same target URL whether
                  it has text or audio).

  POST /voice  -> body: raw 16 kHz / 16-bit / mono PCM (little-endian).
                  Transcribes via openai-whisper, forwards the transcription
                  to OpenClaw, streams the reply back. The first SSE event
                  is ``event: stt`` carrying the transcription so the ESP32
                  can show what it understood before the LLM reply starts.

The reply SSE stream is forwarded verbatim from OpenClaw, so the ESP32 SSE
parser written for Phase 1 keeps working with no changes.

Run::

  pip install -r requirements.txt
  OPENCLAW_TOKEN="<your token>" \
      uvicorn bridge:app --host 0.0.0.0 --port 8000 --log-level info

Bind to ``0.0.0.0`` so the ESP32 on the same LAN can reach it. ``127.0.0.1``
will NOT work for the device.
"""

import json
import os
import struct
import tempfile
import time
from contextlib import asynccontextmanager
from typing import AsyncIterator

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import StreamingResponse, JSONResponse

# =====================================================================
# Config (env vars)
# =====================================================================
OPENCLAW_URL   = os.environ.get("OPENCLAW_URL",   "http://127.0.0.1:18789/v1/responses")
OPENCLAW_TOKEN = os.environ.get("OPENCLAW_TOKEN", "")
AGENT_ID       = os.environ.get("OPENCLAW_AGENT", "main")
OPENCLAW_MODEL = os.environ.get("OPENCLAW_MODEL", "deepseek/deepseek-v4-flash")
USER_ID        = os.environ.get("OPENCLAW_USER",  "esp32-voice")
MAX_TOKENS     = int(os.environ.get("OPENCLAW_MAX_TOKENS", "300"))

WHISPER_MODEL  = os.environ.get("WHISPER_MODEL", "small")
WHISPER_LANG   = os.environ.get("WHISPER_LANG",  "zh")

SAMPLE_RATE     = 16000
BITS_PER_SAMPLE = 16
CHANNELS        = 1

# =====================================================================
# Whisper lifecycle (loaded once at startup, kept warm)
# =====================================================================
_whisper_model = None


def _load_whisper():
    """Load the whisper model lazily; safe to call multiple times."""
    global _whisper_model
    if _whisper_model is None:
        import whisper  # imported here so missing dep gives a clearer error
        print(f"[bridge] loading whisper model: {WHISPER_MODEL!r} ...", flush=True)
        t0 = time.time()
        _whisper_model = whisper.load_model(WHISPER_MODEL)
        print(f"[bridge] whisper ready in {time.time() - t0:.1f}s", flush=True)
    return _whisper_model


@asynccontextmanager
async def lifespan(_app: FastAPI):
    # Pre-warm whisper at startup so the first /voice doesn't pay the
    # 3 s model-load cost.
    _load_whisper()
    yield


app = FastAPI(title="OpenClaw LAN Bridge", lifespan=lifespan)


# =====================================================================
# Helpers
# =====================================================================
def pcm_to_wav_bytes(pcm: bytes) -> bytes:
    """Wrap raw PCM 16 kHz/16-bit/mono into a minimal RIFF/WAVE blob."""
    byte_rate   = SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE // 8
    block_align = CHANNELS * BITS_PER_SAMPLE // 8
    fmt_chunk = struct.pack(
        "<4sIHHIIHH",
        b"fmt ", 16,
        1,                      # PCM
        CHANNELS,
        SAMPLE_RATE,
        byte_rate,
        block_align,
        BITS_PER_SAMPLE,
    )
    data_chunk = struct.pack("<4sI", b"data", len(pcm)) + pcm
    riff = struct.pack(
        "<4sI4s",
        b"RIFF",
        4 + len(fmt_chunk) + len(data_chunk),
        b"WAVE",
    )
    return riff + fmt_chunk + data_chunk


async def stream_openclaw(user_text: str, stt_prefix: bool) -> AsyncIterator[bytes]:
    """Open a streaming POST to OpenClaw and yield the SSE bytes.

    If ``stt_prefix`` is set, the first frame yielded is a synthesized
    ``event: stt`` carrying the transcribed text, so the ESP32 can render
    what was understood before the LLM reply arrives.
    """
    if stt_prefix:
        payload = json.dumps({"text": user_text}, ensure_ascii=False)
        yield f"event: stt\ndata: {payload}\n\n".encode("utf-8")

    body = {
        "model":             "openclaw",
        "input":             user_text,
        "user":              USER_ID,
        "stream":            True,
        "max_output_tokens": MAX_TOKENS,
    }
    headers = {
        "Authorization":       f"Bearer {OPENCLAW_TOKEN}",
        "Content-Type":        "application/json",
        "x-openclaw-agent-id": AGENT_ID,
        "x-openclaw-model":    OPENCLAW_MODEL,
        "Accept":              "text/event-stream",
    }

    timeout = httpx.Timeout(connect=10.0, read=120.0, write=10.0, pool=10.0)
    async with httpx.AsyncClient(timeout=timeout) as client:
        async with client.stream(
            "POST", OPENCLAW_URL, json=body, headers=headers
        ) as resp:
            if resp.status_code != 200:
                detail = (await resp.aread()).decode("utf-8", errors="replace")
                msg = f"openclaw {resp.status_code}: {detail[:500]}"
                err_payload = json.dumps({"message": msg}, ensure_ascii=False)
                yield f"event: error\ndata: {err_payload}\n\n".encode("utf-8")
                return
            async for chunk in resp.aiter_raw():
                if chunk:
                    yield chunk


# =====================================================================
# Routes
# =====================================================================
@app.get("/health")
def health():
    return {
        "status":        "ok",
        "whisper_model": WHISPER_MODEL,
        "openclaw_url":  OPENCLAW_URL,
        "agent":         AGENT_ID,
        "model":         OPENCLAW_MODEL,
    }


@app.post("/chat")
async def chat(req: Request):
    try:
        payload = await req.json()
    except Exception as e:
        return JSONResponse({"error": f"invalid JSON: {e}"}, status_code=400)

    text = (payload.get("text") or "").strip()
    if not text:
        return JSONResponse({"error": "missing 'text' field"}, status_code=400)

    print(f"[bridge] /chat text={text!r}", flush=True)
    return StreamingResponse(
        stream_openclaw(text, stt_prefix=False),
        media_type="text/event-stream",
    )


@app.post("/voice")
async def voice(req: Request):
    pcm = await req.body()
    if not pcm:
        return JSONResponse({"error": "empty body"}, status_code=400)

    seconds = len(pcm) / (SAMPLE_RATE * (BITS_PER_SAMPLE // 8) * CHANNELS)
    print(
        f"[bridge] /voice received {len(pcm)} bytes (~{seconds:.2f}s of audio)",
        flush=True,
    )

    # whisper.transcribe() wants a file path. Dump WAV to a temp file.
    wav = pcm_to_wav_bytes(pcm)
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(wav)
        wav_path = f.name

    try:
        t0 = time.time()
        result = _load_whisper().transcribe(
            wav_path,
            language=WHISPER_LANG,
            fp16=False,  # CPU/MPS path is faster without fp16 fallback noise
        )
        text = (result.get("text") or "").strip()
        print(f"[bridge] STT in {time.time() - t0:.2f}s: {text!r}", flush=True)
    finally:
        try:
            os.unlink(wav_path)
        except OSError:
            pass

    if not text:
        async def _empty():
            yield b'event: stt\ndata: {"text":""}\n\n'
            yield b'event: error\ndata: {"message":"empty transcription"}\n\n'
        return StreamingResponse(_empty(), media_type="text/event-stream")

    return StreamingResponse(
        stream_openclaw(text, stt_prefix=True),
        media_type="text/event-stream",
    )
