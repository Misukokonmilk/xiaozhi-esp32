#include "http_login.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_login";

// HTTP响应数据结构
typedef struct {
    char *buffer;
    int current_len;
} http_response_data_t;

// HTTP事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_response_data_t *response_data = (http_response_data_t *)evt->user_data;
        if (response_data && evt->data_len > 0) {
            char *new_buffer = (char *)realloc(response_data->buffer, response_data->current_len + evt->data_len + 1);
            if (new_buffer) {
                response_data->buffer = new_buffer;
                memcpy(response_data->buffer + response_data->current_len, evt->data, evt->data_len);
                response_data->current_len += evt->data_len;
                response_data->buffer[response_data->current_len] = '\0';
            } else {
                ESP_LOGE(TAG, "Failed to realloc response buffer");
                if (response_data->buffer) {
                    free(response_data->buffer);
                    response_data->buffer = NULL;
                }
                response_data->current_len = 0;
            }
        }
    }
    return ESP_OK;
}

// C风格的回调函数指针
static login_success_callback_t s_c_callback = nullptr;
static HttpLogin* s_http_login_instance = nullptr;

HttpLogin::HttpLogin() 
    : server_host_(HTTP_LOGIN_SERVER_HOST)
    , server_port_(HTTP_LOGIN_SERVER_PORT)
    , login_endpoint_(HTTP_LOGIN_ENDPOINT)
    , login_username_(HTTP_LOGIN_USERNAME)
    , login_password_(HTTP_LOGIN_PASSWORD)
    , token_json_field_(HTTP_LOGIN_TOKEN_JSON_FIELD) {
    s_http_login_instance = this;
}

HttpLogin::~HttpLogin() {
    if (s_http_login_instance == this) {
        s_http_login_instance = nullptr;
    }
}

void HttpLogin::SetServerConfig(const std::string& host, int port, const std::string& endpoint) {
    server_host_ = host;
    server_port_ = port;
    login_endpoint_ = endpoint;
}

void HttpLogin::SetCredentials(const std::string& username, const std::string& password) {
    login_username_ = username;
    login_password_ = password;
}

void HttpLogin::StartTask(login_success_callback_cpp_t on_success_cb) {
    on_success_cb_ = on_success_cb;
    
    // 创建登录任务
    xTaskCreate([](void* arg) {
        HttpLogin* login = (HttpLogin*)arg;
        login->LoginTask();
        vTaskDelete(NULL);
    }, "http_login_task", 8192, this, 5, NULL);
}

void HttpLogin::LoginTask() {
    int retry_count = 0;
    const int max_retries = HTTP_LOGIN_MAX_RETRIES;
    std::string full_url = "http://" + server_host_ + ":" + std::to_string(server_port_) + login_endpoint_;
    
    // 创建JSON请求数据
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "username", login_username_.c_str());
        cJSON_AddStringToObject(root, "password", login_password_.c_str());
    }
    
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!post_data) {
        ESP_LOGE(TAG, "Failed to create JSON post data.");
        return;
    }

    ESP_LOGI(TAG, "Login Task Started. URL: %s", full_url.c_str());
    ESP_LOGI(TAG, "POST Data: %s", post_data);

    while (retry_count < max_retries) {
        char details_buff[32];
        snprintf(details_buff, sizeof(details_buff), "Attempt %d/%d", retry_count + 1, max_retries);
        ESP_LOGI(TAG, "Attempting login: %s", details_buff);
        
        http_response_data_t response_data_ctx = {0};
        esp_http_client_config_t config = {0};
        config.url = full_url.c_str();
        config.method = HTTP_METHOD_POST;
        config.event_handler = http_event_handler;
        config.user_data = &response_data_ctx;
        config.timeout_ms = HTTP_CLIENT_TIMEOUT_MS;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Client-Platform", CLIENT_PLATFORM);
        esp_http_client_set_header(client, "Device-Type", DEVICE_TYPE);
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        
        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "Login attempt finished. ESP-IDF Error: %s (0x%x), HTTP Status: %d", 
                 esp_err_to_name(err), err, status_code);
        if (response_data_ctx.buffer) {
             ESP_LOGI(TAG, "Server Response Body: %s", response_data_ctx.buffer);
        }

        if (err == ESP_OK && (status_code == 200 || status_code == 201)) {
            cJSON *response_json = cJSON_Parse(response_data_ctx.buffer);
            if (response_json) {
                cJSON *token_item = cJSON_GetObjectItemCaseSensitive(response_json, token_json_field_.c_str());
                if (cJSON_IsString(token_item) && (token_item->valuestring != NULL)) {
                    ESP_LOGI(TAG, "Login successful! Token extracted.");
                    ESP_LOGI(TAG, "Token: %s", token_item->valuestring);
                    
                    // 调用C++回调
                    if (on_success_cb_) {
                        on_success_cb_(std::string(token_item->valuestring));
                    }
                    
                    cJSON_Delete(response_json);
                    if (response_data_ctx.buffer) free(response_data_ctx.buffer);
                    esp_http_client_cleanup(client);
                    free(post_data);
                    return;
                } else {
                    ESP_LOGE(TAG, "Token not found in response");
                }
                cJSON_Delete(response_json);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON response");
            }
        } else {
            ESP_LOGE(TAG, "HTTP request failed");
        }
        
        if (response_data_ctx.buffer) free(response_data_ctx.buffer);
        esp_http_client_cleanup(client);
        retry_count++;
        if (retry_count < max_retries) {
            ESP_LOGW(TAG, "Retrying in %d seconds...", HTTP_LOGIN_RETRY_DELAY_MS/1000);
            vTaskDelay(pdMS_TO_TICKS(HTTP_LOGIN_RETRY_DELAY_MS));
        }
    }
    
    ESP_LOGE(TAG, "Login failed after %d retries.", max_retries);
    free(post_data);
}

// C风格的回调包装函数
extern "C" void http_login_start_task(login_success_callback_t on_success_cb) {
    s_c_callback = on_success_cb;
    
    // 创建HttpLogin实例并启动任务
    static HttpLogin login;
    login.StartTask([](const std::string& token) {
        // 将C++回调转换为C回调
        if (s_c_callback) {
            s_c_callback(token.c_str());
        }
    });
}