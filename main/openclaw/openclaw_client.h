#ifndef _OPENCLAW_CLIENT_H_
#define _OPENCLAW_CLIENT_H_

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

#include "protocol.h"   // AudioStreamPacket

/**
 * Minimal client for OpenClaw's OpenAI-Responses-compatible endpoint.
 *
 *   POST http://<host>:<port>/v1/responses
 *     Headers:
 *       Authorization: Bearer <token>
 *       x-openclaw-agent-id: <agent_id>
 *       x-openclaw-model: <model>           (optional override)
 *       Content-Type: application/json
 *     Body:
 *       { "model":"openclaw", "input":"<text>", "user":"<user>",
 *         "stream":true, "max_output_tokens":<n> }
 *
 * Server returns SSE. We only care about:
 *
 *   event: response.output_text.delta
 *   data: {"delta":"<piece>", ...}
 *
 *   data: [DONE]
 *
 * Other event types are ignored.
 *
 * Thread model:
 *   Stream() is blocking and must NOT be called from the main task,
 *   because the SSE callback may need to invoke Schedule() to update UI.
 *   Use a dedicated task (see Application::TriggerOpenclawTest()).
 */
class OpenclawClient {
public:
    struct Config {
        std::string host;             // e.g. "192.168.86.25"
        int         port = 80;        // e.g. 18789
        std::string token;            // Bearer
        std::string agent_id;         // x-openclaw-agent-id
        std::string model;            // x-openclaw-model (empty => omit)
        std::string user;             // session key, e.g. "esp32"
        int         max_output_tokens = 200;
        int         timeout_ms = 60000;
    };

    struct Callbacks {
        // Called per SSE delta chunk (UTF-8 text fragment).
        std::function<void(const std::string& delta)> on_delta;
        // Called once when the stream finishes successfully.
        std::function<void()> on_done;
        // Called on transport / parse / HTTP error.
        std::function<void(const std::string& message)> on_error;
    };

    explicit OpenclawClient(Config cfg);

    // Synchronous: opens HTTP, streams SSE, fires callbacks, then returns.
    // Returns true if stream ended with [DONE] cleanly.
    bool Stream(const std::string& input, const Callbacks& cb);

    // ----- TTS streaming: /v1/audio/speech ---------------------------------
    struct SpeakCallbacks {
        // Called once per Opus frame as it arrives. Caller typically pushes
        // straight into AudioService::PushPacketToDecodeQueue() so playback
        // starts before the whole reply has been synthesized.
        std::function<void(std::unique_ptr<AudioStreamPacket>)> on_packet;
        // Called once when the stream ends cleanly (EOF after a full frame).
        std::function<void()> on_done;
        // Called on transport / HTTP / framing errors.
        std::function<void(const std::string&)> on_error;
    };

    // Synchronous: POST {"input": text} to /v1/audio/speech and read back a
    // stream of length-prefixed Opus frames:
    //   [uint16 BE: N][N bytes opus frame] ...
    //
    // Each frame is 24 kHz / mono / 60 ms, matching the device-side Opus
    // decoder + I2S output pipeline.
    //
    // Returns true if the stream ended cleanly. Blocking — call from a
    // dedicated worker task, not from the main loop.
    bool Speak(const std::string& input, const SpeakCallbacks& cb);

    // Synchronous: POST raw 16 kHz / 16-bit / mono PCM (wrapped in a 44-byte
    // RIFF/WAVE header) as the entire request body to /v1/audio/transcriptions.
    // Per OpenClaw's contract this is NOT multipart — just plain WAV bytes
    // with Content-Type: application/octet-stream, no Authorization, no
    // model/language fields (the server auto-detects). The recognised text
    // is written into *text on success.
    //
    //   POST /v1/audio/transcriptions
    //   Content-Type: application/octet-stream
    //
    //   <WAV bytes>
    //
    //   -> 200 { "text": "..." }
    //   -> 400 { "error": { "message": "Empty body" } }
    //   -> 500 { "error": { "message": "transcription failed" } }
    bool Transcribe(const std::vector<int16_t>& pcm, std::string* text);

private:
    // Forward decl to keep esp_http_client.h out of the header.
    struct HttpEvent;
    static int HttpEventThunk(HttpEvent* evt);
    void OnHttpData(const char* data, int len);
    void ParseSseLine(const std::string& line);
    void DispatchEvent();

    Config cfg_;
    Callbacks cb_;

    // SSE parsing state
    std::string buffer_;          // raw chunk accumulator until '\n'
    std::string current_event_;   // last "event:" seen, cleared on dispatch
    std::string current_data_;    // last "data:" payload seen
    bool done_ = false;
    std::string error_;

    // Extract the value of a top-level "delta" JSON string field.
    // Returns true if found, writes decoded UTF-8 to out.
    static bool ExtractDelta(const std::string& json, std::string* out);
};

#endif  // _OPENCLAW_CLIENT_H_
