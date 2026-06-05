#include "openclaw_client.h"

#include <cJSON.h>
#include <cstdlib>
#include <cstring>
#include <esp_http_client.h>
#include <esp_log.h>

#define TAG "OpenclawClient"

OpenclawClient::OpenclawClient(Config cfg) : cfg_(std::move(cfg)) {}

// esp_http_client expects a C-style callback. The HttpEvent type is just
// an opaque alias declared in the header — here it actually is
// esp_http_client_event_t. We cast user_data back to `this` and let the
// member function do real work.
int OpenclawClient::HttpEventThunk(HttpEvent* evt_opaque) {
    auto* evt = reinterpret_cast<esp_http_client_event_t*>(evt_opaque);
    auto* self = static_cast<OpenclawClient*>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (self && evt->data && evt->data_len > 0) {
                self->OnHttpData(static_cast<const char*>(evt->data), evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool OpenclawClient::Stream(const std::string& input,
                            const Callbacks& cb,
                            const StreamOptions& opts) {
    cb_ = cb;
    buffer_.clear();
    current_event_.clear();
    current_data_.clear();
    done_ = false;
    error_.clear();

    // Route by payload type:
    //   image present  -> /v1/vision (voice-esp32 plugin bypass)
    //   image absent   -> /v1/responses (regular chat path)
    const bool has_image = !opts.image_data_url.empty();
    const char* path = has_image ? "/v1/vision" : "/v1/responses";
    std::string url = "http://" + cfg_.host + ":"
                    + std::to_string(cfg_.port) + path;

    // Build request body via cJSON (handles escaping for us).
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "openclaw");
    cJSON_AddStringToObject(root, "input", input.c_str());
    cJSON_AddStringToObject(root, "user", cfg_.user.c_str());
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_output_tokens", cfg_.max_output_tokens);
    if (has_image) {
        cJSON_AddStringToObject(root, "image", opts.image_data_url.c_str());
    }
    char* body_cstr = cJSON_PrintUnformatted(root);
    std::string body = body_cstr ? body_cstr : "{}";
    cJSON_free(body_cstr);
    cJSON_Delete(root);

    // Don't dump the full body (~70 KB if image attached) — log a short
    // summary instead so the serial log stays readable.
    if (!has_image) {
        ESP_LOGI(TAG, "POST %s body=%s", url.c_str(), body.c_str());
    } else {
        ESP_LOGI(TAG, "POST %s input=%s image=%u bytes (data url) model=%s",
                 url.c_str(), input.c_str(),
                 static_cast<unsigned>(opts.image_data_url.size()),
                 opts.model_override.empty() ? "(default)"
                                             : opts.model_override.c_str());
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = cfg_.timeout_ms;
    http_cfg.event_handler = reinterpret_cast<http_event_handle_cb>(&HttpEventThunk);
    http_cfg.user_data = this;
    http_cfg.disable_auto_redirect = true;
    // RX: chat replies stream incrementally so a modest buffer suffices.
    // TX: vision requests carry a base64 JPEG (~70-120 KB of payload).
    // Without a big TX buffer the body gets flushed in 1 KB chunks, each
    // paying a WiFi round-trip — pushing a typical vision request from
    // ~4 s to >30 s. 32 KB gets a typical body out in one shot.
    http_cfg.buffer_size = 4096;
    http_cfg.buffer_size_tx = 32768;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        if (cb_.on_error) cb_.on_error("esp_http_client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    std::string auth = "Bearer " + cfg_.token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    if (!cfg_.agent_id.empty()) {
        esp_http_client_set_header(client, "x-openclaw-agent-id", cfg_.agent_id.c_str());
    }
    // Model header: per-call override (vision path) wins over the default.
    const std::string& effective_model = !opts.model_override.empty()
                                       ? opts.model_override
                                       : cfg_.model;
    if (!effective_model.empty()) {
        esp_http_client_set_header(client, "x-openclaw-model",
                                   effective_model.c_str());
    }
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    esp_http_client_set_post_field(client, body.c_str(), body.size());

    // perform() drives event callbacks synchronously and blocks until done.
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        std::string msg = std::string("HTTP transport error: ") + esp_err_to_name(err);
        ESP_LOGE(TAG, "%s", msg.c_str());
        if (cb_.on_error) cb_.on_error(msg);
        return false;
    }
    if (status < 200 || status >= 300) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "HTTP status %d", status);
        ESP_LOGE(TAG, "%s", tmp);
        if (cb_.on_error) cb_.on_error(tmp);
        return false;
    }
    if (!error_.empty()) {
        if (cb_.on_error) cb_.on_error(error_);
        return false;
    }

    // Flush any trailing partial line (defensive — server should send final \n).
    if (!buffer_.empty()) {
        ParseSseLine(buffer_);
        buffer_.clear();
    }
    // Dispatch any pending event that wasn't terminated by a blank line.
    if (!current_data_.empty() || !current_event_.empty()) {
        DispatchEvent();
    }

    if (cb_.on_done) cb_.on_done();
    return done_;
}

void OpenclawClient::OnHttpData(const char* data, int len) {
    buffer_.append(data, len);

    size_t pos;
    while ((pos = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        // strip optional \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
            // SSE: blank line = end of one event
            DispatchEvent();
        } else {
            ParseSseLine(line);
        }
    }
}

void OpenclawClient::ParseSseLine(const std::string& line) {
    static const char kEvent[] = "event:";
    static const char kData[]  = "data:";

    auto strip_leading_space = [](const std::string& s, size_t start) {
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
        return s.substr(start);
    };

    if (line.compare(0, sizeof(kEvent) - 1, kEvent) == 0) {
        current_event_ = strip_leading_space(line, sizeof(kEvent) - 1);
    } else if (line.compare(0, sizeof(kData) - 1, kData) == 0) {
        // RFC: multiple data: lines should be joined with '\n'. OpenClaw uses
        // one per event in practice, but handle both.
        std::string val = strip_leading_space(line, sizeof(kData) - 1);
        if (!current_data_.empty()) current_data_ += '\n';
        current_data_ += val;
    }
    // Ignore other fields (id:, retry:, comments).
}

void OpenclawClient::DispatchEvent() {
    if (current_data_.empty() && current_event_.empty()) return;

    // Stream terminators. We accept any of:
    //   data: [DONE]              ← OpenAI-compat, /v1/responses uses this
    //   event: response.completed ← OpenAI Responses canonical, /v1/vision
    //   event: response.failed    ← OpenAI Responses error terminator
    //   event: error              ← generic SSE error
    //
    // The last two also surface the error message via on_error so the UI
    // gets a "what went wrong" toast instead of just an empty reply.
    if (current_data_ == "[DONE]" ||
        current_event_ == "response.completed") {
        done_ = true;
        current_event_.clear();
        current_data_.clear();
        return;
    }
    if (current_event_ == "response.failed" ||
        current_event_ == "error") {
        // Try to dig out a human-readable message from the data JSON.
        std::string msg = "server reported '" + current_event_ + "'";
        if (!current_data_.empty()) {
            cJSON* root = cJSON_Parse(current_data_.c_str());
            if (root) {
                cJSON* err = cJSON_GetObjectItem(root, "error");
                if (err) {
                    cJSON* m = cJSON_GetObjectItem(err, "message");
                    if (cJSON_IsString(m) && m->valuestring) {
                        msg = m->valuestring;
                    }
                }
                cJSON_Delete(root);
            }
        }
        ESP_LOGE(TAG, "stream terminated by server: %s", msg.c_str());
        if (cb_.on_error) cb_.on_error(msg);
        done_ = true;
        current_event_.clear();
        current_data_.clear();
        return;
    }

    if (current_event_ == "response.output_text.delta") {
        std::string delta;
        if (ExtractDelta(current_data_, &delta)) {
            // POC diagnostic: dump raw SSE data and decoded delta so we can
            // tell whether bytes are lost in transport, parsing or display.
            ESP_LOGI(TAG, "sse data=%s -> delta=[%s] (len=%u)",
                     current_data_.c_str(), delta.c_str(),
                     static_cast<unsigned>(delta.size()));
            if (cb_.on_delta) cb_.on_delta(delta);
        } else {
            ESP_LOGW(TAG, "delta event without parseable 'delta' field: %s",
                     current_data_.c_str());
        }
    } else {
        // Useful when debugging unknown events.
        ESP_LOGD(TAG, "ignored sse event='%s' data='%s'",
                 current_event_.c_str(), current_data_.c_str());
    }

    current_event_.clear();
    current_data_.clear();
}

bool OpenclawClient::Speak(const std::string& input, const SpeakCallbacks& cb) {
    if (input.empty()) {
        if (cb.on_error) cb.on_error("empty input");
        return false;
    }

    // Build JSON request body via cJSON so escaping is correct.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "input", input.c_str());
#ifdef CONFIG_OPENCLAW_TTS_VOICE
    if (strlen(CONFIG_OPENCLAW_TTS_VOICE) > 0) {
        cJSON_AddStringToObject(root, "voice", CONFIG_OPENCLAW_TTS_VOICE);
    }
#endif
#ifdef CONFIG_OPENCLAW_TTS_SPEED
    if (strlen(CONFIG_OPENCLAW_TTS_SPEED) > 0) {
        // Server expects a number; let cJSON serialize it. atof handles
        // empty/invalid as 0 (skipped above).
        double speed = atof(CONFIG_OPENCLAW_TTS_SPEED);
        if (speed >= 0.5 && speed <= 2.0) {
            cJSON_AddNumberToObject(root, "speed", speed);
        }
    }
#endif
    char* body_cstr = cJSON_PrintUnformatted(root);
    std::string body = body_cstr ? body_cstr : "{}";
    cJSON_free(body_cstr);
    cJSON_Delete(root);

    const std::string url = "http://" + cfg_.host + ":"
                          + std::to_string(cfg_.port) + "/v1/audio/speech";
    ESP_LOGI(TAG, "POST %s body=%s", url.c_str(), body.c_str());

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = 60000;
    http_cfg.disable_auto_redirect = true;
    // RX: TTS streams Opus frames back; a larger receive buffer cuts the
    // number of read syscalls per second. TX: body is just a small JSON
    // object so the default suffices.
    http_cfg.buffer_size = 4096;
    http_cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        if (cb.on_error) cb.on_error("esp_http_client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    bool ok = false;
    uint32_t frame_count = 0;

    do {
        esp_err_t err = esp_http_client_open(client, body.size());
        if (err != ESP_OK) {
            std::string msg = std::string("open failed: ") + esp_err_to_name(err);
            ESP_LOGE(TAG, "%s", msg.c_str());
            if (cb.on_error) cb.on_error(msg);
            break;
        }

        int w = esp_http_client_write(client, body.data(),
                                      static_cast<int>(body.size()));
        if (w != static_cast<int>(body.size())) {
            ESP_LOGE(TAG, "write body failed: %d/%u", w,
                     static_cast<unsigned>(body.size()));
            if (cb.on_error) cb.on_error("write body failed");
            break;
        }

        int64_t total = esp_http_client_fetch_headers(client);
        int status   = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS response status=%d content-length=%d",
                 status, (int)total);

        if (status < 200 || status >= 300) {
            char buf[256];
            int rn = esp_http_client_read(client, buf, sizeof(buf));
            std::string snippet(buf, rn > 0 ? rn : 0);
            char msg[160];
            snprintf(msg, sizeof(msg), "HTTP %d: %.100s",
                     status, snippet.c_str());
            ESP_LOGE(TAG, "%s", msg);
            if (cb.on_error) cb.on_error(msg);
            break;
        }

        // Helper: read exactly n bytes, looping over short reads.
        // Returns: n  on full read, 0 on clean EOF before any bytes,
        //          -1 on error / partial read.
        auto read_exact = [&client](uint8_t* dst, int n) -> int {
            int got = 0;
            while (got < n) {
                int r = esp_http_client_read(client, (char*)(dst + got), n - got);
                if (r == 0) return got;       // EOF
                if (r < 0)  return -1;
                got += r;
            }
            return got;
        };

        // Stream loop: [uint16 BE len][len bytes opus] ...
        bool stream_error = false;
        while (true) {
            uint8_t len_bytes[2];
            int got = read_exact(len_bytes, 2);
            if (got == 0) {
                ok = true;        // clean EOF
                break;
            }
            if (got != 2) {
                ESP_LOGE(TAG, "short read on frame header (%d)", got);
                if (cb.on_error) cb.on_error("short read on frame header");
                stream_error = true;
                break;
            }

            uint16_t frame_len = (static_cast<uint16_t>(len_bytes[0]) << 8)
                               |  static_cast<uint16_t>(len_bytes[1]);
            if (frame_len == 0 || frame_len > 1500) {
                char msg[64];
                snprintf(msg, sizeof(msg), "invalid frame length: %u",
                         static_cast<unsigned>(frame_len));
                ESP_LOGE(TAG, "%s", msg);
                if (cb.on_error) cb.on_error(msg);
                stream_error = true;
                break;
            }

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate    = 24000;
            packet->frame_duration = 60;
            packet->payload.resize(frame_len);
            int body_got = read_exact(packet->payload.data(), frame_len);
            if (body_got != frame_len) {
                ESP_LOGE(TAG, "short read on frame body: %d/%u",
                         body_got, static_cast<unsigned>(frame_len));
                if (cb.on_error) cb.on_error("short read on frame body");
                stream_error = true;
                break;
            }

            ++frame_count;
            if (cb.on_packet) cb.on_packet(std::move(packet));
        }

        ESP_LOGI(TAG, "TTS stream done: %u frames (~%.2fs of audio)",
                 static_cast<unsigned>(frame_count),
                 frame_count * 0.06f);
        if (ok && cb.on_done) cb.on_done();
        (void)stream_error;
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool OpenclawClient::ExtractDelta(const std::string& json, std::string* out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    bool ok = false;
    cJSON* d = cJSON_GetObjectItem(root, "delta");
    if (cJSON_IsString(d) && d->valuestring) {
        out->assign(d->valuestring);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

namespace {

// Build a minimal 44-byte RIFF/WAVE header for 16 kHz / 16-bit / mono PCM.
void AppendWavHeader(std::string* out, uint32_t pcm_bytes) {
    constexpr uint16_t kChannels  = 1;
    constexpr uint32_t kSampleRate = 16000;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint32_t kByteRate = kSampleRate * kChannels * kBitsPerSample / 8;
    constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;

    auto u16 = [out](uint16_t v) {
        char b[2] = { char(v & 0xff), char((v >> 8) & 0xff) };
        out->append(b, 2);
    };
    auto u32 = [out](uint32_t v) {
        char b[4] = {
            char(v & 0xff), char((v >> 8) & 0xff),
            char((v >> 16) & 0xff), char((v >> 24) & 0xff),
        };
        out->append(b, 4);
    };

    out->append("RIFF", 4);
    u32(36 + pcm_bytes);   // ChunkSize
    out->append("WAVE", 4);
    out->append("fmt ", 4);
    u32(16);               // Subchunk1Size
    u16(1);                // AudioFormat = PCM
    u16(kChannels);
    u32(kSampleRate);
    u32(kByteRate);
    u16(kBlockAlign);
    u16(kBitsPerSample);
    out->append("data", 4);
    u32(pcm_bytes);
}

}  // namespace

bool OpenclawClient::Transcribe(const std::vector<int16_t>& pcm,
                                std::string* text) {
    text->clear();

    if (pcm.empty()) {
        ESP_LOGW(TAG, "Transcribe called with empty PCM");
        return false;
    }

    const uint32_t pcm_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));

    // Per OpenClaw contract: the entire body is just a WAV blob — no
    // multipart, no extra form fields, no Authorization. The 44-byte
    // RIFF/WAVE header is followed by raw PCM samples.
    std::string wav_header;
    AppendWavHeader(&wav_header, pcm_bytes);

    const size_t content_length = wav_header.size() + pcm_bytes;

    const std::string url = "http://" + cfg_.host + ":"
                          + std::to_string(cfg_.port)
                          + "/v1/audio/transcriptions";

    ESP_LOGI(TAG, "POST %s content-length=%u (%u PCM bytes ~ %.2fs)",
             url.c_str(), static_cast<unsigned>(content_length),
             static_cast<unsigned>(pcm_bytes), pcm_bytes / 32000.0f);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = 60000;
    http_cfg.disable_auto_redirect = true;
    // RX: STT response is tiny ({"text":"..."}). TX: WAV upload averages
    // 30-70 KB for typical utterances; 8 KB gets the body out in ~5-10
    // sends instead of the 30-70 sends needed at 1 KB.
    http_cfg.buffer_size = 4096;
    http_cfg.buffer_size_tx = 8192;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type",
                               "application/octet-stream");

    bool ok = false;
    do {
        esp_err_t err = esp_http_client_open(client, content_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
            break;
        }

        // Helper: write all bytes of a buffer, log if short write.
        auto write_all = [&](const char* label, const char* data, size_t len) -> bool {
            int w = esp_http_client_write(client, data, static_cast<int>(len));
            if (w != static_cast<int>(len)) {
                ESP_LOGE(TAG, "write %s failed: %d/%u", label, w,
                         static_cast<unsigned>(len));
                return false;
            }
            return true;
        };

        // WAV header (44 bytes) then PCM streamed in small chunks so we
        // don't allocate a giant contiguous send buffer.
        if (!write_all("wav header", wav_header.data(),
                       wav_header.size())) break;

        const char* pcm_ptr = reinterpret_cast<const char*>(pcm.data());
        size_t remaining = pcm_bytes;
        const size_t kChunk = 2048;
        bool pcm_ok = true;
        while (remaining > 0) {
            int n = static_cast<int>(remaining < kChunk ? remaining : kChunk);
            int w = esp_http_client_write(client, pcm_ptr, n);
            if (w != n) {
                ESP_LOGE(TAG, "write pcm failed: %d/%d", w, n);
                pcm_ok = false;
                break;
            }
            pcm_ptr += n;
            remaining -= n;
        }
        if (!pcm_ok) break;

        int64_t total = esp_http_client_fetch_headers(client);
        int status   = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "STT response status=%d content-length=%d",
                 status, (int)total);

        std::string body;
        body.reserve(256);
        char buf[256];
        while (true) {
            int r = esp_http_client_read(client, buf, sizeof(buf));
            if (r <= 0) break;
            body.append(buf, r);
        }

        if (status < 200 || status >= 300) {
            ESP_LOGE(TAG, "STT non-2xx: %d body=%.200s", status, body.c_str());
            break;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            ESP_LOGE(TAG, "STT response not JSON: %.200s", body.c_str());
            break;
        }
        cJSON* t = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(t) && t->valuestring) {
            text->assign(t->valuestring);
            ok = true;
            ESP_LOGI(TAG, "STT text=%s", text->c_str());
        } else {
            ESP_LOGE(TAG, "STT response missing 'text' field: %.200s",
                     body.c_str());
        }
        cJSON_Delete(root);
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}
