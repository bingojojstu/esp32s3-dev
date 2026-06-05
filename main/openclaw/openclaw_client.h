#ifndef _OPENCLAW_CLIENT_H_
#define _OPENCLAW_CLIENT_H_

#include <string>
#include <functional>
#include <atomic>

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

    // Synchronous: POST raw PCM (16 kHz, 16-bit, mono) wrapped in a WAV
    // container as multipart/form-data to /v1/audio/transcriptions. The
    // recognised text is written into *text on success.
    //
    // OpenAI-compatible request:
    //   POST /v1/audio/transcriptions
    //   Authorization: Bearer <token>
    //   Content-Type: multipart/form-data; boundary=...
    //
    //   --boundary
    //   Content-Disposition: form-data; name="model"
    //
    //   whisper-1
    //   --boundary
    //   Content-Disposition: form-data; name="language"
    //
    //   zh
    //   --boundary
    //   Content-Disposition: form-data; name="file"; filename="audio.wav"
    //   Content-Type: audio/wav
    //
    //   <WAV bytes>
    //   --boundary--
    //
    // Reply: { "text": "..." }
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
