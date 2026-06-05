#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <atomic>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#ifdef CONFIG_USE_OPENCLAW_BACKEND
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#endif

#define TAG "Application"


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

#ifdef CONFIG_USE_OPENCLAW_BACKEND
    // AudioCodec defaults to volume=70 which is ~0.49x amplitude — quiet
    // through a MAX98357A driving a small speaker. Apply the configured
    // default for the OpenClaw POC right after the codec is initialized.
    if (codec) {
        codec->SetOutputVolume(CONFIG_OPENCLAW_OUTPUT_VOLUME);
        ESP_LOGI("Application", "Output volume set to %d",
                 CONFIG_OPENCLAW_OUTPUT_VOLUME);
    }
#endif

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
#ifdef CONFIG_USE_OPENCLAW_BACKEND
    // POC mode: bypass Xiaozhi cloud entirely. No OTA check, no activation
    // handshake, no chat protocol. The device just goes idle and waits for
    // BOOT-press to fire an OpenClaw HTTP request.
    //
    // Sync wall-clock time from public NTP so logs aren't stuck in 1970 and
    // any component that timestamps records (audio_debugger, TLS, etc.)
    // sees a sane year. Non-fatal if it fails — we just keep going.
    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp_cfg.start = true;
        esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
        if (err == ESP_OK) {
            // Wait up to 5s for first sync; don't block boot forever.
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) == ESP_OK) {
                ESP_LOGI("Application", "SNTP time synced");
            } else {
                ESP_LOGW("Application", "SNTP did not sync in 5s (continuing)");
            }
        } else {
            ESP_LOGW("Application", "esp_netif_sntp_init failed: %s",
                     esp_err_to_name(err));
        }
    }

    // We still create a dummy Ota object only so HandleActivationDoneEvent()
    // can read GetCurrentVersion() without crashing.
    ota_ = std::make_unique<Ota>();
    has_server_time_ = true;
    ESP_LOGW("Application", "OpenClaw backend enabled — skipping Xiaozhi activation");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
    return;
#else
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
#endif
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
#ifdef CONFIG_USE_OPENCLAW_WAKE_WORD
    // Hands-free voice: a wake word event triggers the same path as a
    // BOOT long-press. StartOpenclawVoice() is re-entrancy-guarded so a
    // wake firing while a request is in flight is safely dropped.
    ESP_LOGI(TAG, "Wake word detected — starting OpenClaw voice worker");
    StartOpenclawVoice();
    return;
#endif
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

#ifdef CONFIG_USE_OPENCLAW_BACKEND
#include "openclaw_client.h"
#ifdef CONFIG_USE_OPENCLAW_VISION
#include <mbedtls/base64.h>
#include "camera.h"
#endif

namespace {

// Re-entrancy guard. The voice worker and the text "death-letter" worker
// share this so a long-press during an in-flight reply is ignored cleanly.
static std::atomic<bool> g_openclaw_in_flight{false};

// Voice recording state. Set true by StartOpenclawVoice (BOOT long-press),
// cleared by StopOpenclawVoice (BOOT release). The worker watches it.
static std::atomic<bool> g_voice_recording{false};

#ifdef CONFIG_USE_OPENCLAW_VISION
// Live-preview state. The preview task runs while recording: it calls
// Camera::Capture() in a loop, which in turn pushes the frame into the
// LcdDisplay's preview widget via SetPreviewImage (see Capture() code).
// When the voice worker is ready to stop, it clears g_preview_active.
// The task sets g_preview_running back to false on exit so the worker
// knows it can safely call CaptureToJpeg() without racing on current_fb_.
static std::atomic<bool> g_preview_active{false};
static std::atomic<bool> g_preview_running{false};

// Returns true if any of CONFIG_OPENCLAW_VISION_KEYWORDS appears as a
// substring in `text`. Whitespace around tokens is trimmed.
static bool TextHasVisionKeyword(const std::string& text) {
    static const char* kKeywords = CONFIG_OPENCLAW_VISION_KEYWORDS;
    if (text.empty() || kKeywords == nullptr || kKeywords[0] == '\0') {
        return false;
    }
    const char* p = kKeywords;
    while (*p) {
        const char* end = strchr(p, ',');
        if (end == nullptr) end = p + strlen(p);
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        const char* tok_end = end;
        while (tok_end > p && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) --tok_end;
        if (tok_end > p) {
            std::string token(p, tok_end - p);
            if (text.find(token) != std::string::npos) {
                ESP_LOGI("Application", "vision keyword matched: %s", token.c_str());
                return true;
            }
        }
        p = (*end == '\0') ? end : (end + 1);
    }
    return false;
}

// Wrap raw JPEG bytes in an OpenAI-style data URL.
static std::string JpegToDataUrl(const std::vector<uint8_t>& jpeg) {
    static const char kPrefix[] = "data:image/jpeg;base64,";
    if (jpeg.empty()) return "";
    size_t need = 0;
    mbedtls_base64_encode(nullptr, 0, &need, jpeg.data(), jpeg.size());
    std::string out;
    out.resize(sizeof(kPrefix) - 1 + need);
    memcpy(out.data(), kPrefix, sizeof(kPrefix) - 1);
    size_t written = 0;
    int rc = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(out.data() + sizeof(kPrefix) - 1),
        need, &written, jpeg.data(), jpeg.size());
    if (rc != 0) {
        ESP_LOGE("Application", "base64 encode failed: %d", rc);
        return "";
    }
    out.resize(sizeof(kPrefix) - 1 + written);
    return out;
}

// Camera preview task: loops Capture() at the configured FPS. Camera's
// own Capture() implementation calls display->SetPreviewImage so the LCD
// reflects what the camera sees in near-real-time without any extra
// plumbing here. Exits cleanly when g_preview_active goes false.
static void OpenclawPreviewTask(void*) {
    auto* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        g_preview_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    const int frame_period_ms = 1000 / CONFIG_OPENCLAW_PREVIEW_FPS;
    while (g_preview_active.load()) {
        if (!camera->Capture()) {
            // Camera transiently failed — back off briefly and try again.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(frame_period_ms));
    }
    g_preview_running.store(false);
    vTaskDelete(nullptr);
}

// Called by the voice worker AFTER recording finishes. Stops the preview
// task, captures one final frame to JPEG, and returns a data URL if the
// vision path should be used.
//
// Vision path is used when:
//   * CONFIG_OPENCLAW_ALWAYS_ATTACH_IMAGE is set, OR
//   * the STT transcript contains a vision keyword.
static std::string FinalizeVisionAfterRecording(const std::string& stt_text) {
    // Tell preview to stop and wait for it.
    g_preview_active.store(false);
    int waited = 0;
    while (g_preview_running.load() && waited < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (g_preview_running.load()) {
        ESP_LOGW("Application",
                 "preview task didn't exit after 500ms; proceeding anyway");
    }

#ifdef CONFIG_OPENCLAW_ALWAYS_ATTACH_IMAGE
    const bool want_image = true;
#else
    const bool want_image = TextHasVisionKeyword(stt_text);
#endif
    if (!want_image) return "";

    auto* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) return "";

    // One last fresh capture so the JPEG matches "what user was pointing
    // at when they finished talking" — the preview task may have updated
    // mid-utterance.
    std::vector<uint8_t> jpeg;
    if (!camera->CaptureToJpeg(jpeg) || jpeg.empty()) {
        ESP_LOGW("Application", "final vision capture failed");
        return "";
    }
    std::string url = JpegToDataUrl(jpeg);
    ESP_LOGI("Application",
             "Attaching image: %u bytes raw JPEG -> %u bytes data url",
             static_cast<unsigned>(jpeg.size()),
             static_cast<unsigned>(url.size()));
    return url;
}
#endif  // CONFIG_USE_OPENCLAW_VISION

// Session counter. CONFIG_OPENCLAW_USER is the base; we suffix this counter
// onto it so the same firmware can start fresh conversations on demand.
// Bumped by Application::ResetOpenclawSession(); read by MakeOpenclawConfig.
//
// Persisted to NVS (namespace "openclaw", key "session_seq") so that
// rebooting the device does NOT reuse old session IDs that openclaw
// already remembers. Without this, every "esp32-s3-s1" after a reboot
// would silently re-enter the previous boot's first new session.
static std::atomic<uint32_t> g_openclaw_session_seq{0};
static std::atomic<bool>     g_seq_loaded{false};

static const char kSeqNamespace[] = "openclaw";
static const char kSeqKey[]       = "session_seq";

static void EnsureSeqLoaded() {
    if (g_seq_loaded.exchange(true)) return;  // already loaded
    Settings settings(kSeqNamespace, /*readwrite=*/false);
    int32_t persisted = settings.GetInt(kSeqKey, 0);
    if (persisted > 0) {
        g_openclaw_session_seq.store(static_cast<uint32_t>(persisted));
        ESP_LOGI("Application",
                 "Restored OpenClaw session seq from NVS: %u",
                 static_cast<unsigned>(persisted));
    }
}

static std::string CurrentOpenclawUserId() {
    EnsureSeqLoaded();
    uint32_t seq = g_openclaw_session_seq.load();
    if (seq == 0) {
        return CONFIG_OPENCLAW_USER;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-s%u",
             CONFIG_OPENCLAW_USER, static_cast<unsigned>(seq));
    return buf;
}

// Build a config struct from Kconfig values; same shape for both paths.
static OpenclawClient::Config MakeOpenclawConfig() {
    OpenclawClient::Config cfg;
    cfg.host = CONFIG_OPENCLAW_HOST;
    cfg.port = CONFIG_OPENCLAW_PORT;
    cfg.token = CONFIG_OPENCLAW_TOKEN;
    cfg.agent_id = CONFIG_OPENCLAW_AGENT_ID;
    cfg.model = CONFIG_OPENCLAW_MODEL;
    cfg.user = CurrentOpenclawUserId();
    cfg.max_output_tokens = CONFIG_OPENCLAW_MAX_OUTPUT_TOKENS;
    return cfg;
}

// Open SSE stream, accumulate deltas, render final reply, and return the
// accumulated text so the caller can pipe it on to TTS.
// Returns an empty string on error / [DONE] with no content.
// opts.image_data_url + opts.model_override are forwarded; when the image
// is set the request goes to /v1/vision (see OpenclawClient::Stream).
static std::string StreamOpenclawReplyAndDisplay(
        OpenclawClient& client,
        const std::string& user_text,
        Display* display,
        const OpenclawClient::StreamOptions& opts = {}) {
    std::string accumulated;
    uint32_t delta_count = 0;

    OpenclawClient::Callbacks cb;
    cb.on_delta = [&accumulated, &delta_count](const std::string& delta) {
        accumulated.append(delta);
        ++delta_count;
    };
    cb.on_done = [&accumulated, &delta_count, display]() {
        ESP_LOGI("Application",
                 "OpenClaw reply complete: %u deltas, %u bytes total",
                 static_cast<unsigned>(delta_count),
                 static_cast<unsigned>(accumulated.size()));
        ESP_LOGI("Application", "Full reply: %s", accumulated.c_str());
        if (!display) return;
        if (accumulated.empty()) {
            display->SetChatMessage("assistant", "[empty response]");
        } else {
            display->SetChatMessage("assistant", accumulated.c_str());
        }
    };
    cb.on_error = [display](const std::string& msg) {
        ESP_LOGE("Application", "OpenClaw error: %s", msg.c_str());
        if (display) display->ShowNotification(msg.c_str(), 5000);
    };

    client.Stream(user_text, cb, opts);
    return accumulated;
}

#ifdef CONFIG_USE_OPENCLAW_TTS
// Post the LLM's final reply text to /v1/audio/speech and pipe the
// returned Opus frames straight into AudioService's decode queue so the
// existing Opus decoder + I2S task pair plays them through the speaker.
// Blocking; runs from the same worker that did Stream().
static void SpeakOpenclawReply(OpenclawClient& client,
                               const std::string& text,
                               Display* display) {
    if (text.empty()) return;
    auto& audio = Application::GetInstance().GetAudioService();

    uint32_t frames = 0;
    OpenclawClient::SpeakCallbacks cb;
    cb.on_packet = [&audio, &frames](std::unique_ptr<AudioStreamPacket> p) {
        // wait=true backpressures the HTTP read loop so PSRAM doesn't fill
        // up if the decoder/I2S falls behind.
        audio.PushPacketToDecodeQueue(std::move(p), /*wait=*/true);
        ++frames;
    };
    cb.on_error = [display](const std::string& err) {
        ESP_LOGE("Application", "TTS error: %s", err.c_str());
        if (display) display->ShowNotification(err.c_str(), 3000);
    };

    bool ok = client.Speak(text, cb);
    ESP_LOGI("Application", "TTS %s, %u frames queued",
             ok ? "complete" : "failed", static_cast<unsigned>(frames));

    // Drain the playback queue before the worker returns so a fast follow-up
    // BOOT press doesn't talk over the in-flight reply.
    audio.WaitForPlaybackQueueEmpty();
}
#endif

// === Text path (BOOT short-press) ====================================
struct OpenclawJob {
    std::string prompt;
};

static void OpenclawTextWorker(void* arg) {
    std::unique_ptr<OpenclawJob> job(static_cast<OpenclawJob*>(arg));
    auto* display = Board::GetInstance().GetDisplay();

    if (display) display->SetChatMessage("user", job->prompt.c_str());

    OpenclawClient client(MakeOpenclawConfig());
    std::string reply = StreamOpenclawReplyAndDisplay(client, job->prompt, display);

#ifdef CONFIG_USE_OPENCLAW_TTS
    SpeakOpenclawReply(client, reply, display);
#endif

    g_openclaw_in_flight = false;
    vTaskDelete(nullptr);
}

// === Voice path (BOOT long-press) ====================================
//
// The whole round-trip (record -> STT -> stream reply) runs in one worker
// task. Recording stops when g_voice_recording goes false (set by
// StopOpenclawVoice from the BOOT release handler).

static constexpr int kVoiceSampleRate     = 16000;
static constexpr int kVoiceChunkSamples   = 1024;       // ~64 ms per chunk
static constexpr int kVoiceMaxSeconds     = 15;
static constexpr int kVoiceMaxSamples     = kVoiceSampleRate * kVoiceMaxSeconds;
static constexpr int kVoiceMinSamples     = kVoiceSampleRate / 4;  // 0.25 s

// Per-chunk average of |sample|. Cheap proxy for RMS for energy VAD.
static int AverageAbsSample(const std::vector<int16_t>& chunk) {
    if (chunk.empty()) return 0;
    int64_t sum = 0;
    for (int16_t s : chunk) sum += (s < 0 ? -s : s);
    return static_cast<int>(sum / chunk.size());
}

static void OpenclawVoiceWorker(void* arg) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto* display = board.GetDisplay();
    auto& audio = app.GetAudioService();

    // -------- Phase 0: own the codec --------------------------------------
    // If wake-word detection was running it was also reading the codec —
    // disable it while we record so there's no contention.
    audio.EnableWakeWordDetection(false);

#ifdef CONFIG_USE_OPENCLAW_VISION
    // Start the camera preview loop in parallel — it pushes frames into
    // the LCD's preview widget at CONFIG_OPENCLAW_PREVIEW_FPS so the user
    // sees what the camera sees while they speak.
    g_preview_active.store(true);
    g_preview_running.store(true);
    if (xTaskCreatePinnedToCore(&OpenclawPreviewTask, "openclaw_cam",
                                4096 * 2, nullptr, 4, nullptr,
                                /*core=*/0) != pdPASS) {
        ESP_LOGW("Application",
                 "Failed to spawn preview task (continuing without preview)");
        g_preview_active.store(false);
        g_preview_running.store(false);
    }
#endif

    if (display) {
        display->ShowNotification("\xF0\x9F\x8E\xA4 Listening...", 30000);
    }

    // -------- Phase 1: record + inline energy VAD -----------------------
    // Two-phase state machine:
    //   1. WAITING — recording started, user hasn't begun talking yet.
    //      Allow up to kVadInitialWaitMs of silence before giving up.
    //   2. SPEAKING — speech detected at least once. End recording
    //      when sustained silence (>= kVadEndSilenceMs) accumulates,
    //      gated by at least kVadMinSpeechMs of speech so a brief
    //      noise hit + immediate silence doesn't end us.
    //
    // We also exit when:
    //   * g_voice_recording is cleared (BOOT release / explicit stop)
    //   * Recording length hits the absolute cap (kVoiceMaxSeconds)
    std::vector<int16_t> pcm;
    pcm.reserve(kVoiceMaxSamples);

    constexpr int kChunkMs = (kVoiceChunkSamples * 1000) / kVoiceSampleRate;
#ifdef CONFIG_USE_OPENCLAW_WAKE_WORD
    const int kVadThreshold       = CONFIG_OPENCLAW_VAD_RMS_THRESHOLD;
    const int kVadMinSpeechMs     = CONFIG_OPENCLAW_VAD_MIN_SPEECH_MS;
    const int kVadEndSilenceMs    = CONFIG_OPENCLAW_VAD_SILENCE_MS;
    const int kVadInitialWaitMs   = CONFIG_OPENCLAW_VAD_INITIAL_WAIT_MS;
#else
    // VAD parameters are #ifdef'd onto USE_OPENCLAW_WAKE_WORD because
    // their Kconfig entries are too — when wake word is off the worker
    // is BOOT-only and just runs to user release.
    const int kVadThreshold       = 0;
    const int kVadMinSpeechMs     = 0;
    const int kVadEndSilenceMs    = 0;
    const int kVadInitialWaitMs   = 0;
#endif
    const bool vad_enabled = (kVadThreshold > 0);

    enum VadPhase { WAITING_FOR_SPEECH, SPEAKING };
    VadPhase phase = WAITING_FOR_SPEECH;
    int initial_silent_ms = 0;
    int end_silent_ms = 0;
    int speech_ms = 0;

    while (g_voice_recording.load() &&
           static_cast<int>(pcm.size()) < kVoiceMaxSamples) {
        std::vector<int16_t> chunk;
        if (!audio.ReadAudioData(chunk, kVoiceSampleRate, kVoiceChunkSamples)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (vad_enabled) {
            const int avg_abs = AverageAbsSample(chunk);
            const bool is_speech = (avg_abs > kVadThreshold);

            if (is_speech) {
                speech_ms += kChunkMs;
                end_silent_ms = 0;
                if (phase == WAITING_FOR_SPEECH) {
                    phase = SPEAKING;
                    ESP_LOGI("Application", "VAD: speech started");
                }
            } else {
                if (phase == WAITING_FOR_SPEECH) {
                    initial_silent_ms += kChunkMs;
                    if (initial_silent_ms >= kVadInitialWaitMs) {
                        ESP_LOGI("Application",
                                 "VAD: no speech after %d ms, giving up",
                                 initial_silent_ms);
                        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
                        break;
                    }
                } else {
                    end_silent_ms += kChunkMs;
                    if (speech_ms >= kVadMinSpeechMs &&
                        end_silent_ms >= kVadEndSilenceMs) {
                        ESP_LOGI("Application",
                                 "VAD: end of speech "
                                 "(%d ms total speech, %d ms trailing silence)",
                                 speech_ms, end_silent_ms);
                        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
                        break;
                    }
                }
            }
        }

        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
    }

    ESP_LOGI("Application",
             "Voice capture done: %u samples (~%.2fs)",
             static_cast<unsigned>(pcm.size()),
             pcm.size() / float(kVoiceSampleRate));

    // Clear the recording flag in case we exited via VAD rather than user
    // releasing the button — the BOOT-press code path checks it.
    g_voice_recording.store(false);

    if (static_cast<int>(pcm.size()) < kVoiceMinSamples) {
        ESP_LOGW("Application", "Voice clip too short, ignoring");
        if (display) display->ShowNotification("Too short", 2000);
#ifdef CONFIG_USE_OPENCLAW_VISION
        g_preview_active.store(false);
#endif
        audio.EnableWakeWordDetection(true);
        g_openclaw_in_flight = false;
        vTaskDelete(nullptr);
        return;
    }

    // -------- Phase 2: STT ---------------------------------------------
    if (display) display->ShowNotification("Transcribing...", 30000);

    OpenclawClient client(MakeOpenclawConfig());
    std::string text;
    bool stt_ok = client.Transcribe(pcm, &text);
    // Free PCM ASAP — Stream() will need RAM for SSE buffering.
    pcm.clear();
    pcm.shrink_to_fit();

    if (!stt_ok || text.empty()) {
        const char* msg = stt_ok ? "Empty transcription" : "STT failed";
        ESP_LOGW("Application", "%s", msg);
        if (display) display->ShowNotification(msg, 3000);
#ifdef CONFIG_USE_OPENCLAW_VISION
        g_preview_active.store(false);
#endif
        audio.EnableWakeWordDetection(true);
        g_openclaw_in_flight = false;
        vTaskDelete(nullptr);
        return;
    }

    if (display) display->SetChatMessage("user", text.c_str());

    // -------- Phase 3: optional vision (camera attachment) -------------
    OpenclawClient::StreamOptions stream_opts;
#ifdef CONFIG_USE_OPENCLAW_VISION
    stream_opts.image_data_url = FinalizeVisionAfterRecording(text);
    if (!stream_opts.image_data_url.empty()) {
        stream_opts.model_override = CONFIG_OPENCLAW_VISION_MODEL;
        if (display) {
            display->ShowNotification("\xF0\x9F\x91\x80 sending image...", 2000);
        }
    }
#endif

    // -------- Phase 4: stream LLM reply --------------------------------
    std::string reply = StreamOpenclawReplyAndDisplay(client, text, display, stream_opts);

#ifdef CONFIG_USE_OPENCLAW_TTS
    // -------- Phase 5: TTS playback ------------------------------------
    SpeakOpenclawReply(client, reply, display);
#endif

    // -------- Phase 6: re-arm wake word for the next utterance ----------
    audio.EnableWakeWordDetection(true);

    g_openclaw_in_flight = false;
    vTaskDelete(nullptr);
}

}  // namespace

void Application::TriggerOpenclawTest(const std::string& prompt) {
    bool expected = false;
    if (!g_openclaw_in_flight.compare_exchange_strong(expected, true)) {
        ESP_LOGW("Application", "OpenClaw request already in flight, ignoring");
        return;
    }
    auto* job = new OpenclawJob{prompt};
    BaseType_t ok = xTaskCreate(&OpenclawTextWorker, "openclaw_text",
                                4096 * 2, job, 4, nullptr);
    if (ok != pdPASS) {
        delete job;
        g_openclaw_in_flight = false;
        ESP_LOGE("Application", "Failed to spawn OpenClaw text worker");
    }
}

void Application::StartOpenclawVoice() {
    bool expected = false;
    if (!g_openclaw_in_flight.compare_exchange_strong(expected, true)) {
        ESP_LOGW("Application",
                 "Voice ignored: another OpenClaw request is in flight");
        return;
    }
    g_voice_recording.store(true);

    // The voice worker spawns the camera preview task itself (after the
    // codec is owned), so StartOpenclawVoice doesn't need to do that
    // here. Wake-word and BOOT paths converge to the same worker.
    BaseType_t ok = xTaskCreate(&OpenclawVoiceWorker, "openclaw_voice",
                                4096 * 3, nullptr, 4, nullptr);
    if (ok != pdPASS) {
        g_voice_recording.store(false);
        g_openclaw_in_flight = false;
        ESP_LOGE("Application", "Failed to spawn OpenClaw voice worker");
    }
}

void Application::StopOpenclawVoice() {
    // Signal the worker. It finishes recording, then runs STT + LLM stream
    // on its own thread. Safe to call from any task.
    g_voice_recording.store(false);
}

void Application::ResetOpenclawSession() {
    EnsureSeqLoaded();
    uint32_t next = g_openclaw_session_seq.fetch_add(1) + 1;

    // Persist BEFORE displaying so even a power-loss-during-toast can't
    // strand us on a reused ID next boot.
    Settings settings(kSeqNamespace, /*readwrite=*/true);
    settings.SetInt(kSeqKey, static_cast<int32_t>(next));

    std::string new_id = std::string(CONFIG_OPENCLAW_USER) + "-s" + std::to_string(next);
    ESP_LOGI("Application", "OpenClaw session reset -> %s (persisted)", new_id.c_str());
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        std::string msg = "New session: " + new_id;
        display->ShowNotification(msg.c_str(), 3000);
        // Also clear the visible chat so it's obvious context was wiped.
        display->ClearChatMessages();
    }
}
#endif  // CONFIG_USE_OPENCLAW_BACKEND

