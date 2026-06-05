#include "openclaw_client.h"

#include <cJSON.h>
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

bool OpenclawClient::Stream(const std::string& input, const Callbacks& cb) {
    cb_ = cb;
    buffer_.clear();
    current_event_.clear();
    current_data_.clear();
    done_ = false;
    error_.clear();

    // Build URL: http://host:port/v1/responses
    std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.port) + "/v1/responses";

    // Build request body via cJSON (handles escaping for us).
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "openclaw");
    cJSON_AddStringToObject(root, "input", input.c_str());
    cJSON_AddStringToObject(root, "user", cfg_.user.c_str());
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_output_tokens", cfg_.max_output_tokens);
    char* body_cstr = cJSON_PrintUnformatted(root);
    std::string body = body_cstr ? body_cstr : "{}";
    cJSON_free(body_cstr);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "POST %s body=%s", url.c_str(), body.c_str());

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = cfg_.timeout_ms;
    http_cfg.event_handler = reinterpret_cast<http_event_handle_cb>(&HttpEventThunk);
    http_cfg.user_data = this;
    http_cfg.disable_auto_redirect = true;
    http_cfg.buffer_size = 1024;
    http_cfg.buffer_size_tx = 1024;

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
    if (!cfg_.model.empty()) {
        esp_http_client_set_header(client, "x-openclaw-model", cfg_.model.c_str());
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

    // Stream terminator
    if (current_data_ == "[DONE]") {
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
