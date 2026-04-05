#include "http_refresh.h"
#include "config.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>
#include <string>

static const char *TAG = "http_refresh";

typedef struct {
    char *buffer;
    int current_len;
} http_response_data_t;

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

HttpRefresh::HttpRefresh()
    : server_host_(HTTP_LOGIN_SERVER_HOST),
      server_port_(HTTP_LOGIN_SERVER_PORT),
      refresh_endpoint_(HTTP_REFRESH_ENDPOINT) {}

HttpRefresh::~HttpRefresh() {}

void HttpRefresh::SetServerConfig(const std::string& host, int port, const std::string& endpoint) {
    server_host_ = host;
    server_port_ = port;
    refresh_endpoint_ = endpoint;
}

void HttpRefresh::StartTask(refresh_success_callback_cpp_t on_success_cb) {
    on_success_cb_ = on_success_cb;
    xTaskCreate([](void* arg) {
        HttpRefresh* self = (HttpRefresh*)arg;
        self->RefreshTask();
        vTaskDelete(NULL);
    }, "http_refresh_task", 8192, this, 5, NULL);
}

void HttpRefresh::RefreshTask() {
    Settings ws_settings("websocket", false);
    std::string token = ws_settings.GetString("token");
    if (token.empty()) {
        ESP_LOGE(TAG, "No existing token in Settings; skip refresh");
        return;
    }

    std::string full_url = "http://" + server_host_ + ":" + std::to_string(server_port_) + refresh_endpoint_;
    ESP_LOGI(TAG, "Refresh Task Started. URL: %s", full_url.c_str());

    for (int retry = 0; retry < HTTP_REFRESH_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Retrying refresh (%d/%d)...", retry + 1, HTTP_REFRESH_MAX_RETRIES);
        }

        http_response_data_t response_data_ctx = {0};
        esp_http_client_config_t config = {0};
        config.url = full_url.c_str();
        config.method = HTTP_METHOD_POST;
        config.event_handler = http_event_handler;
        config.user_data = &response_data_ctx;
        config.timeout_ms = HTTP_CLIENT_TIMEOUT_MS;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        std::string auth_header = std::string("Bearer ") + token;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
        esp_http_client_set_header(client, "Client-Platform", CLIENT_PLATFORM);
        esp_http_client_set_header(client, "Device-Type", DEVICE_TYPE);

        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "Refresh attempt finished. ESP-IDF Error: %s (0x%x), HTTP Status: %d", 
                 esp_err_to_name(err), err, status_code);
        if (response_data_ctx.buffer) {
            ESP_LOGI(TAG, "Server Response Body: %s", response_data_ctx.buffer);
        }

        if (err == ESP_OK && (status_code == 200 || status_code == 201)) {
            cJSON *response_json = cJSON_Parse(response_data_ctx.buffer);
            if (response_json) {
                cJSON *token_item = cJSON_GetObjectItemCaseSensitive(response_json, HTTP_LOGIN_TOKEN_JSON_FIELD);
                if (cJSON_IsString(token_item) && (token_item->valuestring != NULL)) {
                    ESP_LOGI(TAG, "Token refresh successful!");
                    if (on_success_cb_) {
                        on_success_cb_(std::string(token_item->valuestring));
                    }
                    cJSON_Delete(response_json);
                    if (response_data_ctx.buffer) free(response_data_ctx.buffer);
                    esp_http_client_cleanup(client);
                    return; // Success — exit task
                } else {
                    ESP_LOGE(TAG, "Token not found in refresh response");
                }
                cJSON_Delete(response_json);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON refresh response");
            }
        } else if (status_code == 401 || status_code == 403) {
            // Auth failure — no point retrying
            ESP_LOGE(TAG, "Auth failure (%d); not retrying", status_code);
            if (response_data_ctx.buffer) free(response_data_ctx.buffer);
            esp_http_client_cleanup(client);
            return;
        } else {
            // Transient error (5xx, timeout, network) — will retry
            ESP_LOGE(TAG, "HTTP refresh request failed (transient)");
        }

        if (response_data_ctx.buffer) free(response_data_ctx.buffer);
        esp_http_client_cleanup(client);

        if (retry < HTTP_REFRESH_MAX_RETRIES - 1) {
            vTaskDelay(pdMS_TO_TICKS(HTTP_REFRESH_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "Token refresh failed after %d retries.", HTTP_REFRESH_MAX_RETRIES);
}

extern "C" void http_refresh_token_start_task(refresh_success_callback_t on_success_cb) {
    static HttpRefresh refresher;
    refresher.StartTask([on_success_cb](const std::string& token) {
        if (on_success_cb) {
            on_success_cb(token.c_str());
        }
    });
}