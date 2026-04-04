#ifndef _PROTOCOLS_CONFIG_H_
#define _PROTOCOLS_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// HTTP Login Server Configuration
#define HTTP_LOGIN_SERVER_HOST "47.109.195.63"
#define HTTP_LOGIN_SERVER_PORT 17777
// HTTP Login endpoint and credentials (Starfire server)
#define HTTP_LOGIN_ENDPOINT "/login"
#define HTTP_LOGIN_USERNAME "sf11"
#define HTTP_LOGIN_PASSWORD "123456"
#define HTTP_LOGIN_TOKEN_JSON_FIELD "token"
// HTTP Refresh endpoint path
#define HTTP_REFRESH_ENDPOINT "/auth/refresh"
// Refresh server URL: fetch new token from this endpoint
#define HTTP_REFRESH_SERVER_HOST HTTP_LOGIN_SERVER_HOST

// WebSocket Server Configuration
// 配置WebSocket服务器地址和协议版本
#define WEBSOCKET_PROTOCOL_VERSION 1
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define HTTP_SCHEME "http"
#define WS_SCHEME "ws"
#define WEBSOCKET_SERVER_URL WS_SCHEME "://" HTTP_LOGIN_SERVER_HOST ":" TOSTRING(HTTP_LOGIN_SERVER_PORT) "/ws"

// HTTP Login Retry Configuration
// 配置HTTP登录重试次数和重试间隔
#define HTTP_LOGIN_MAX_RETRIES 5                        // 最大重试次数
#define HTTP_LOGIN_RETRY_DELAY_MS 5000                  // 重试间隔（毫秒）

// WebSocket Connection Configuration
// 配置WebSocket连接参数
#define WEBSOCKET_CONNECTION_TIMEOUT_MS 10000           // WebSocket连接超时时间（毫秒）
#define WEBSOCKET_PING_INTERVAL_SECONDS 30              // WebSocket心跳间隔（秒）

// Platform & Device Type
#define CLIENT_PLATFORM "xiaozhi-esp32"
#define DEVICE_TYPE "miaoban"

// Audio Configuration
// 配置音频参数
#define AUDIO_FORMAT "opus"                             // 音频编码格式
#define AUDIO_SAMPLE_RATE 16000                         // 音频采样率
#define AUDIO_CHANNELS 1                                // 音频通道数
#define AUDIO_FRAME_DURATION_MS 60                      // 音频帧时长（毫秒）

// HTTP Client Configuration
// 配置HTTP客户端参数
#define HTTP_CLIENT_TIMEOUT_MS 10000

// OTA Check Endpoint
// 与 HTTP/WS 同一服务器，仅协议与路径不同
#define OTA_CHECK_ENDPOINT "/ota/"

#ifdef __cplusplus
}
#endif

#endif // _PROTOCOLS_CONFIG_H_