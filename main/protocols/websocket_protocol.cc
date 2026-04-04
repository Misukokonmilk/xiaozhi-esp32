#include "websocket_protocol.h"
#include "config.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <network_interface.h>
#include "assets/lang_config.h"

#define TAG "WS"
#define CONNECTION_TIMEOUT_MS WEBSOCKET_CONNECTION_TIMEOUT_MS

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    version_ = WEBSOCKET_PROTOCOL_VERSION;
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    ESP_LOGD(TAG, "SendAudio: version=%d, payload=%u bytes", (int)version_, (unsigned)packet->payload.size());
    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    // 只使用本地宏定义的WebSocket服务器地址
    std::string url = WEBSOCKET_SERVER_URL;
    std::string token = "";
    int version = WEBSOCKET_PROTOCOL_VERSION;
    return OpenAudioChannelWithToken(url, token, version);
}

bool WebsocketProtocol::OpenAudioChannelWithToken(const std::string& url, const std::string& token, int version) {
    version_ = version;

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    // 构造持久字符串，避免传递临时对象的 c_str() 导致潜在悬空指针问题
    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        std::string token_copy = token; // 创建可修改的副本
        if (token_copy.find(" ") == std::string::npos) {
            token_copy = "Bearer " + token_copy;
        }
        websocket_->SetHeader("Authorization", token_copy.c_str());
    }
    std::string protocol_version_str = std::to_string(version_);
    std::string device_id_str = SystemInfo::GetMacAddress();
    std::string client_id_str = Board::GetInstance().GetUuid();
    websocket_->SetHeader("Protocol-Version", protocol_version_str.c_str());
    websocket_->SetHeader("Device-Id", device_id_str.c_str());
    websocket_->SetHeader("Client-Id", client_id_str.c_str());
    websocket_->SetHeader("Client-Platform", CLIENT_PLATFORM);
    websocket_->SetHeader("Device-Type", DEVICE_TYPE);

    // 连接前打印诊断信息，便于定位握手问题
    ESP_LOGI(TAG, "WS diagnostics: url=%s, has_token=%s, token_len=%d, device_id=%s, client_id=%s",
        url.c_str(),
        token.empty() ? "false" : "true",
        (int)token.size(),
        SystemInfo::GetMacAddress().c_str(),
        Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // ESP_LOGI(TAG, "Received binary audio data, size: %d bytes", len);
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    // BinaryProtocol2: [u16 version][u16 type][u32 reserved][u32 timestamp][u32 payload_size][payload]
                    // Use memcpy + ntoh* to avoid alignment and endianness issues
                    if (len < sizeof(uint16_t) * 2 + sizeof(uint32_t) * 3) {
                        ESP_LOGE(TAG, "Invalid v2 header size: %d", len);
                        return;
                    }
                    size_t offset = 0;
                    uint16_t version_be = 0, type_be = 0;
                    uint32_t reserved_be = 0, timestamp_be = 0, payload_size_be = 0;
                    memcpy(&version_be, data + offset, sizeof(version_be));
                    offset += sizeof(version_be);
                    memcpy(&type_be, data + offset, sizeof(type_be));
                    offset += sizeof(type_be);
                    memcpy(&reserved_be, data + offset, sizeof(reserved_be));
                    offset += sizeof(reserved_be);
                    memcpy(&timestamp_be, data + offset, sizeof(timestamp_be));
                    offset += sizeof(timestamp_be);
                    memcpy(&payload_size_be, data + offset, sizeof(payload_size_be));
                    offset += sizeof(payload_size_be);

                    uint32_t payload_size = ntohl(payload_size_be);
                    uint32_t timestamp = ntohl(timestamp_be);
                    if (payload_size == 0) {
                        ESP_LOGW(TAG, "Empty v2 payload, drop");
                        return;
                    }
                    if (len < offset + payload_size) {
                        ESP_LOGE(TAG, "Truncated v2 packet: header=%d, len=%d, payload=%lu", (int)offset, (int)len, (unsigned long)payload_size);
                        return;
                    }

                    const uint8_t* payload_start = reinterpret_cast<const uint8_t*>(data) + offset;
                    auto packet = std::make_unique<AudioStreamPacket>();
                    packet->sample_rate = AUDIO_SAMPLE_RATE;
                    packet->frame_duration = AUDIO_FRAME_DURATION_MS;
                    packet->timestamp = timestamp;
                    packet->payload.assign(payload_start, payload_start + payload_size);
                    on_incoming_audio_(std::move(packet));
                } else if (version_ == 3) {
                    // BinaryProtocol3: [u8 type][u8 reserved][u16 payload_size][payload]
                    if (len < 4) {
                        ESP_LOGE(TAG, "Invalid v3 header size: %d", len);
                        return;
                    }
                    uint8_t type = 0, reserved = 0;
                    uint16_t size_be = 0;
                    size_t offset = 0;
                    memcpy(&type, data + offset, sizeof(type));
                    offset += sizeof(type);
                    memcpy(&reserved, data + offset, sizeof(reserved));
                    offset += sizeof(reserved);
                    memcpy(&size_be, data + offset, sizeof(size_be));
                    offset += sizeof(size_be);
                    uint16_t payload_size = ntohs(size_be);
                    if (payload_size == 0) {
                        ESP_LOGW(TAG, "Empty v3 payload, drop");
                        return;
                    }
                    if (len < offset + payload_size) {
                        ESP_LOGE(TAG, "Truncated v3 packet: header=%d, len=%d, payload=%u", (int)offset, (int)len, payload_size);
                        return;
                    }
                    const uint8_t* payload_start = reinterpret_cast<const uint8_t*>(data) + offset;
                    auto packet = std::make_unique<AudioStreamPacket>();
                    packet->sample_rate = AUDIO_SAMPLE_RATE;
                    packet->frame_duration = AUDIO_FRAME_DURATION_MS;
                    packet->timestamp = 0;
                    packet->payload.assign(payload_start, payload_start + payload_size);
                    on_incoming_audio_(std::move(packet));
                } else {
                    // 移除每次都打印的详细日志
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = AUDIO_SAMPLE_RATE,
                        .frame_duration = AUDIO_FRAME_DURATION_MS,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            } else {
                ESP_LOGW(TAG, "Received audio data but no callback registered");
            }
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", WEBSOCKET_PROTOCOL_VERSION);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", AUDIO_FORMAT);
    cJSON_AddNumberToObject(audio_params, "sample_rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio_params, "channels", AUDIO_CHANNELS);
    cJSON_AddNumberToObject(audio_params, "frame_duration", AUDIO_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
