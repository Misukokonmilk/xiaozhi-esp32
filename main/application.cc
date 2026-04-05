#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "protocols/http_refresh.h"
#include "protocols/http_login.h"
#include "ota.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <time.h>
#include <lwip/apps/sntp.h>
#include <wifi_station.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

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

void Application::CheckAssetsVersion() {
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
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("idle");
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
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(protocol_.get());
                Settings ws_settings("websocket", false);
                std::string token = ws_settings.GetString("token");
                bool ok = false;
                if (ws_protocol) {
                    if (!token.empty()) {
                        ok = ws_protocol->OpenAudioChannelWithToken(websocket_server_url_, token, WEBSOCKET_PROTOCOL_VERSION);
                    } else {
                        ESP_LOGW(TAG, "Token is empty; initiating HTTP login to fetch global token");
                        http_login_start_task([](const char* new_token) {
                            // Persist token
                            Settings ws_settings_cb("websocket", true);
                            ws_settings_cb.SetString("token", new_token);

                            // After login succeeded, open WS and enter listening
                            Application::GetInstance().Schedule([new_token_str = std::string(new_token)]() {
                                auto& app = Application::GetInstance();
                                auto* ws_p = dynamic_cast<WebsocketProtocol*>(app.protocol_.get());
                                if (!ws_p) return;
                                ESP_LOGI(TAG, "Connecting WebSocket with freshly obtained token");
                                if (!ws_p->OpenAudioChannelWithToken(app.websocket_server_url_, new_token_str.c_str(), WEBSOCKET_PROTOCOL_VERSION)) {
                                    ESP_LOGE(TAG, "WebSocket connect failed after login");
                                    return;
                                }
                                app.SetListeningMode(app.aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
                            });
                        });
                        return; // wait login callback to continue
                    }
                } else {
                    ok = protocol_->OpenAudioChannel();
                }
                if (!ok) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            closing_by_user_ = true;
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(protocol_.get());
                Settings ws_settings("websocket", false);
                std::string token = ws_settings.GetString("token");
                bool ok = false;
                if (ws_protocol) {
                    if (!token.empty()) {
                        ok = ws_protocol->OpenAudioChannelWithToken(websocket_server_url_, token, WEBSOCKET_PROTOCOL_VERSION);
                    } else {
                        ESP_LOGW(TAG, "Token is empty when opening WebSocket; falling back to no-token");
                        ok = ws_protocol->OpenAudioChannelWithToken(websocket_server_url_, "", WEBSOCKET_PROTOCOL_VERSION);
                    }
                } else {
                    ok = protocol_->OpenAudioChannel();
                }
                if (!ok) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}


void Application::OnLoginSuccess(const char* token) {
    ESP_LOGI(TAG, "Login successful, token: %s", token);
    // Persist token to NVS for future refresh
    {
        Settings ws_settings("websocket", true);
        ws_settings.SetString("token", token);
    }
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();  // 添加codec声明
    
    // 初始化WebSocket协议
    protocol_ = std::make_unique<WebsocketProtocol>();
    // 创建协议后立刻设置到 AudioService，避免空指针导致后续协调缺失
    audio_service_.SetProtocol(protocol_.get());
    
    // 设置协议回调
    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        ESP_LOGE(TAG, "Network error: %s", message.c_str());
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);

        // 若在连接/监听阶段发生网络错误（包括握手失败），尝试刷新token后重连一次
        if (device_state_ == kDeviceStateConnecting || device_state_ == kDeviceStateListening) {
            http_refresh_token_start_task([](const char* new_token) {
                // Persist refreshed token
                Settings ws_settings("websocket", true);
                ws_settings.SetString("token", new_token);

                // Reconnect using refreshed token
                Application::GetInstance().Schedule([new_token_str = std::string(new_token)]() {
                    auto& app = Application::GetInstance();
                    if (!app.protocol_) return;
                    auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(app.protocol_.get());
                    if (!ws_protocol) return;
                    ESP_LOGI(TAG, "Retrying WebSocket connect after token refresh");
                    if (ws_protocol->OpenAudioChannelWithToken(app.websocket_server_url_, new_token_str.c_str())) {
                        SystemInfo::PrintHeapStats();
                        app.SetDeviceState(kDeviceStateIdle);
                        app.ws_reconnect_delay_seconds_ = 1; // 重置退避延迟
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetChatMessage("system", "网络错误后刷新并重连成功");
                        app.audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
                    } else {
                        ESP_LOGE(TAG, "Reconnect after network error with refreshed token failed");
                        app.Alert(Lang::Strings::ERROR, "网络错误后重连失败", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                        app.ws_reconnect_delay_seconds_ = (app.ws_reconnect_delay_seconds_ * 2 < app.kMaxReconnectDelay) ? app.ws_reconnect_delay_seconds_ * 2 : app.kMaxReconnectDelay;
                    }
                });
            });
        }
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        // 移除每次接收音频包都打印的详细日志
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        } else {
            // 只在设备状态不匹配时打印警告
            static DeviceState last_warned_state = kDeviceStateUnknown;
            if (device_state_ != last_warned_state) {
                ESP_LOGW(TAG, "Device is not in speaking state (%s), dropping audio packets", STATE_STRINGS[device_state_]);
                last_warned_state = device_state_;
            }
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);

        // 手动关闭：不触发刷新/重连，回到空闲
        if (closing_by_user_) {
            ESP_LOGI(TAG, "Audio channel closed by user; skipping token refresh");
            closing_by_user_ = false;
            Schedule([this]() {
                if (device_state_ == kDeviceStateListening || device_state_ == kDeviceStateConnecting) {
                    SetDeviceState(kDeviceStateIdle);
                }
            });
            return;
        }

        // 若Wi‑Fi未连接，延迟重连，等待链路恢复
        if (!WifiStation::GetInstance().IsConnected()) {
            ESP_LOGW(TAG, "Wi‑Fi link down; defer websocket reconnect until Wi‑Fi restores");
            ws_reconnect_waiting_wifi_ = true;
                    ws_reconnect_next_tick_ = clock_ticks_ + ws_reconnect_delay_seconds_; // 使用退避延迟
            Schedule([this]() {
                auto display = Board::GetInstance().GetDisplay();
                display->SetChatMessage("system", "网络断开，等待Wi‑Fi恢复后重连");
            });
            return;
        }

        // 非手动关闭：统一尝试自动重连（包括 idle 等状态）
        DeviceState closed_state = device_state_;
        // 在重连前设置为 connecting，以触发 OnNetworkError 的刷新重试逻辑
        SetDeviceState(kDeviceStateConnecting);
            Settings ws_settings_chk("websocket", false);
            std::string current_token = ws_settings_chk.GetString("token");

            if (current_token.empty()) {
                ESP_LOGW(TAG, "Token empty on channel close; initiating login before reconnect");
                http_login_start_task([](const char* new_token) {
                    // Persist new token
                    Settings ws_settings_cb("websocket", true);
                    ws_settings_cb.SetString("token", new_token);

                    // Reconnect using freshly obtained token
                    Application::GetInstance().Schedule([new_token_str = std::string(new_token)]() {
                        auto& app = Application::GetInstance();
                        if (!app.protocol_) return;
                        auto* ws_p = dynamic_cast<WebsocketProtocol*>(app.protocol_.get());
                        if (!ws_p) return;
                        ESP_LOGI(TAG, "Reconnecting WebSocket after login");
                        if (ws_p->OpenAudioChannelWithToken(app.websocket_server_url_, new_token_str.c_str(), WEBSOCKET_PROTOCOL_VERSION)) {
                            SystemInfo::PrintHeapStats();
                            app.SetDeviceState(kDeviceStateIdle);
                            app.ws_reconnect_delay_seconds_ = 1; // 重置退避延迟
                            auto display = Board::GetInstance().GetDisplay();
                            display->SetChatMessage("system", "登录并重连成功");
                            app.audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
                        } else {
                            ESP_LOGE(TAG, "Reconnect after login failed");
                            app.Alert(Lang::Strings::ERROR, "登录后重连失败", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                            app.ws_reconnect_delay_seconds_ = (app.ws_reconnect_delay_seconds_ * 2 < app.kMaxReconnectDelay) ? app.ws_reconnect_delay_seconds_ * 2 : app.kMaxReconnectDelay;
                        }
                    });
                });
                return; // 登录后重连由上面的逻辑负责，跳过回到空闲兜底
            } else {
                ESP_LOGW(TAG, "Unexpected channel close; attempting auto-reconnect with existing token");
                // 直接用当前token重连，不触发刷新；如握手失败，将在 OnNetworkError 分支处理刷新
                Application::GetInstance().Schedule([current_token]() {
                    auto& app = Application::GetInstance();
                    if (!app.protocol_) return;
                    auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(app.protocol_.get());
                    if (!ws_protocol) return;
                    if (ws_protocol->OpenAudioChannelWithToken(app.websocket_server_url_, current_token.c_str(), WEBSOCKET_PROTOCOL_VERSION)) {
                        SystemInfo::PrintHeapStats();
                        app.SetDeviceState(kDeviceStateIdle);
                        app.ws_reconnect_delay_seconds_ = 1; // 重置退避延迟
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetChatMessage("system", "自动重连成功");
                        app.audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
                    } else {
                        ESP_LOGE(TAG, "Auto-reconnect failed; will fall back to idle and await OnNetworkError handling");
                        app.Alert(Lang::Strings::ERROR, "自动重连失败", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                        app.SetDeviceState(kDeviceStateIdle);
                        app.ws_reconnect_delay_seconds_ = (app.ws_reconnect_delay_seconds_ * 2 < app.kMaxReconnectDelay) ? app.ws_reconnect_delay_seconds_ * 2 : app.kMaxReconnectDelay;
                    }
                });
                return; // 跳过下面的回到空闲兜底，由上面的逻辑负责
            }
        
        // 默认回到空闲
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
                ESP_LOGI(TAG, "Received TTS start event");
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "Received TTS stop event");
                // 按用户语义：stop 表示不再有后续音频，但需播放队列自然结束
                Schedule([this]() {
                    speaking_end_pending_ = true;
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
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
    
    // 启动协议，但不在空闲时常驻连接；仅在需要时（唤醒/开始监听）再建立WS
    if (protocol_->Start()) {
        SetDeviceState(kDeviceStateIdle);
        display->SetChatMessage("system", "已登录，等待唤醒或开始监听");
        ESP_LOGI(TAG, "Protocol started without opening WebSocket; will connect on demand");
    } else {
        ESP_LOGE(TAG, "Failed to start WebSocket protocol");
        Alert(Lang::Strings::ERROR, "协议启动失败", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
    }
}

static void InitSntpOnce() {
    static bool sntp_started = false;
    if (sntp_started) return;

    // 设置时区为中国标准时间（UTC+8），若需其他时区可在设置中调整
    setenv("TZ", "CST-8", 1);
    tzset();

    // 配置与启动 SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_init();
    ESP_LOGI(TAG, "SNTP initialized (server: ntp.aliyun.com)");

    sntp_started = true;
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

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
    
    // 设置Protocol对象到AudioService，用于背压控制
    audio_service_.SetProtocol(protocol_.get());

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // 启动 SNTP 时间同步（一次性）
    InitSntpOnce();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // 开机检查新版本并自动升级
    Ota ota;
    CheckNewVersion(ota);

    // 继续登录与协议初始化
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
    http_login_start_task([](const char* token) {
        std::string token_str(token);
        Application::GetInstance().Schedule([token_str]() {
            Application::GetInstance().OnLoginSuccess(token_str.c_str());
        });
    });
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    ESP_LOGE(TAG, "SendAudio failed, breaking send loop");
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
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

            // 若已收到 stop，等待播放队列耗尽后再切换为 listen/idle
            if (speaking_end_pending_ && device_state_ == kDeviceStateSpeaking && audio_service_.IsIdle()) {
                speaking_end_pending_ = false;
                if (listening_mode_ == kListeningModeManualStop) {
                    SetDeviceState(kDeviceStateIdle);
                } else {
                    SetDeviceState(kDeviceStateListening);
                }
            }

            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }

            // 如果处于等待Wi‑Fi恢复后重连的状态，则在Wi‑Fi恢复时尝试重连
            if (ws_reconnect_waiting_wifi_) {
                if (WifiStation::GetInstance().IsConnected() && clock_ticks_ >= ws_reconnect_next_tick_) {
                    ESP_LOGI(TAG, "Wi‑Fi restored; trying websocket auto‑reconnect");
                    auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(protocol_.get());
                    Settings ws_settings("websocket", false);
                    std::string token = ws_settings.GetString("token");
                    bool ok = false;
                    if (ws_protocol) {
                        ok = ws_protocol->OpenAudioChannelWithToken(websocket_server_url_, token, WEBSOCKET_PROTOCOL_VERSION);
                    } else if (protocol_) {
                        ok = protocol_->OpenAudioChannel();
                    }
                    if (ok) {
                        SystemInfo::PrintHeapStats();
                        SetDeviceState(kDeviceStateIdle);
                        display->SetChatMessage("system", "Wi‑Fi恢复后自动重连成功");
                        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
                        ws_reconnect_waiting_wifi_ = false;
                        ws_reconnect_delay_seconds_ = 1; // 重置退避延迟
                    } else {
                        ESP_LOGW(TAG, "Websocket reconnect failed after Wi‑Fi restore; will retry later");
                        display->SetChatMessage("system", "自动重连失败，稍后重试");
                        ws_reconnect_delay_seconds_ = (ws_reconnect_delay_seconds_ * 2 < kMaxReconnectDelay) ? ws_reconnect_delay_seconds_ * 2 : kMaxReconnectDelay;
                        ws_reconnect_next_tick_ = clock_ticks_ + ws_reconnect_delay_seconds_;
                        ESP_LOGW(TAG, "Reconnect failed; next attempt in %d seconds", ws_reconnect_delay_seconds_);
                    }
                }
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            auto* ws_protocol = dynamic_cast<WebsocketProtocol*>(protocol_.get());
            Settings ws_settings("websocket", false);
            std::string token = ws_settings.GetString("token");
                bool ok = false;
                if (ws_protocol) {
                    if (!token.empty()) {
                        ok = ws_protocol->OpenAudioChannelWithToken(websocket_server_url_, token, WEBSOCKET_PROTOCOL_VERSION);
                    } else {
                        ESP_LOGW(TAG, "Token is empty; initiating HTTP login to fetch global token");
                        http_login_start_task([](const char* new_token) {
                            Settings ws_settings_cb("websocket", true);
                            ws_settings_cb.SetString("token", new_token);

                            Application::GetInstance().Schedule([new_token_str = std::string(new_token)]() {
                                auto& app = Application::GetInstance();
                                auto* ws_p = dynamic_cast<WebsocketProtocol*>(app.protocol_.get());
                                if (!ws_p) return;
                                ESP_LOGI(TAG, "Connecting WebSocket with freshly obtained token");
                                if (!ws_p->OpenAudioChannelWithToken(app.websocket_server_url_, new_token_str.c_str(), WEBSOCKET_PROTOCOL_VERSION)) {
                                    ESP_LOGE(TAG, "WebSocket connect failed after login");
                                    app.audio_service_.EnableWakeWordDetection(true);
                                    return;
                                }
                                auto wake_word = app.audio_service_.GetLastWakeWord();
                                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
                                while (auto packet = app.audio_service_.PopWakeWordPacket()) {
                                    app.protocol_->SendAudio(std::move(packet));
                                }
                                app.protocol_->SendWakeWordDetected(wake_word);
                                app.SetListeningMode(app.aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
                                app.SetListeningMode(app.aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
                                app.audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
                            });
                        });
                        return; // wait login callback to proceed wake-word flow
                    }
                } else {
                    ok = protocol_->OpenAudioChannel();
                }
                if (!ok) {
                    audio_service_.EnableWakeWordDetection(true);
                    return;
                }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
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

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
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
            // 恢复原始行为：进入监听态无条件告知服务器并开启语音处理
            ESP_LOGI(TAG, "Sending listen/start (mode=%d)", (int)listening_mode_);
            protocol_->SendStartListening(listening_mode_);
            ESP_LOGI(TAG, "Enable voice processing for listening state");
            audio_service_.EnableVoiceProcessing(true);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            // 恢复原始行为：进入说话态即初始化解码器
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        closing_by_user_ = true;
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// 已移除OTA相关功能

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                bool short_enough = wake_word.size() <= 12;
                bool no_space = wake_word.find(' ') == std::string::npos;
                bool no_paren = wake_word.find('(') == std::string::npos && wake_word.find(')') == std::string::npos;
                bool no_punct = wake_word.find('，') == std::string::npos && wake_word.find('。') == std::string::npos
                                && wake_word.find(',') == std::string::npos && wake_word.find('.') == std::string::npos
                                && wake_word.find('!') == std::string::npos && wake_word.find('?') == std::string::npos;
                if (short_enough && no_space && no_paren && no_punct) {
                    protocol_->SendWakeWordDetected(wake_word);
                }
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                closing_by_user_ = true;
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
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

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
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
            closing_by_user_ = true;
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}
void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }
            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::OGG_EXCLAMATION);
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::OGG_UPGRADE);
            vTaskDelay(pdMS_TO_TICKS(3000));
            SetDeviceState(kDeviceStateUpgrading);
            display->SetEmotion("download");
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                std::thread([display, progress, speed]() {
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                }).detach();
            });

            if (!upgrade_success) {
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start();
                board.SetPowerSaveMode(true);
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::OGG_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                Reboot();
                return;
            }
        }

        ota.MarkCurrentVersionValid();
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        break;
    }
}
