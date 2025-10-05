#ifndef _PROTOCOLS_CONFIG_H_
#define _PROTOCOLS_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// HTTP Login Server Configuration
// 配置HTTP登录服务器的地址、端口、登录接口路径以及登录凭据
// #define HTTP_LOGIN_SERVER_HOST "47.109.29.58"           // HTTP登录服务器主机地址
// #define HTTP_LOGIN_SERVER_PORT 17777                    // HTTP登录服务器端口
#define HTTP_LOGIN_SERVER_HOST "47.109.195.63"           // HTTP登录服务器主机地址
#define HTTP_LOGIN_SERVER_PORT 17777                    // HTTP登录服务器端口
// #define HTTP_LOGIN_SERVER_HOST "192.168.31.120"           // HTTP登录服务器主机地址
// #define HTTP_LOGIN_SERVER_PORT 8000                    // HTTP登录服务器端口
#define HTTP_LOGIN_ENDPOINT "/login"                    // 登录接口路径
#define HTTP_LOGIN_USERNAME "sf7"                       // 登录用户名
#define HTTP_LOGIN_PASSWORD "123456"                    // 登录密码
#define HTTP_LOGIN_TOKEN_JSON_FIELD "token"             // 服务器返回的token字段名

// WebSocket Server Configuration
// 配置WebSocket服务器地址和协议版本
#define WEBSOCKET_SERVER_URL "ws://47.109.195.63:17777/ws" // WebSocket服务器地址
#define WEBSOCKET_PROTOCOL_VERSION 1                     // WebSocket协议版本
// #define WEBSOCKET_SERVER_URL "ws://192.168.31.120:8000/ws" // WebSocket服务器地址
// #define WEBSOCKET_PROTOCOL_VERSION 1                     // WebSocket协议版本

// HTTP Login Retry Configuration
// 配置HTTP登录重试次数和重试间隔
#define HTTP_LOGIN_MAX_RETRIES 5                        // 最大重试次数
#define HTTP_LOGIN_RETRY_DELAY_MS 5000                  // 重试间隔（毫秒）

// WebSocket Connection Configuration
// 配置WebSocket连接参数
#define WEBSOCKET_CONNECTION_TIMEOUT_MS 10000           // WebSocket连接超时时间（毫秒）
#define WEBSOCKET_PING_INTERVAL_SECONDS 30              // WebSocket心跳间隔（秒）

// Audio Configuration
// 配置音频参数
#define AUDIO_FORMAT "opus"                             // 音频编码格式
#define AUDIO_SAMPLE_RATE 16000                         // 音频采样率
#define AUDIO_CHANNELS 1                                // 音频通道数
#define AUDIO_FRAME_DURATION_MS 60                      // 音频帧时长（毫秒）

// HTTP Client Configuration
// 配置HTTP客户端参数
#define HTTP_CLIENT_TIMEOUT_MS 10000                    // HTTP客户端超时时间（毫秒）

#ifdef __cplusplus
}
#endif

#endif // _PROTOCOLS_CONFIG_H_