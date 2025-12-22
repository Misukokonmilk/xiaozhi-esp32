#ifndef _HTTP_REFRESH_H_
#define _HTTP_REFRESH_H_

#include <functional>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*refresh_success_callback_t)(const char *new_token);

void http_refresh_token_start_task(refresh_success_callback_t on_success_cb);

#ifdef __cplusplus
}

typedef std::function<void(const std::string& new_token)> refresh_success_callback_cpp_t;

class HttpRefresh {
public:
    HttpRefresh();
    ~HttpRefresh();
    void StartTask(refresh_success_callback_cpp_t on_success_cb);
    void SetServerConfig(const std::string& host, int port, const std::string& endpoint);
private:
    std::string server_host_;
    int server_port_;
    std::string refresh_endpoint_;
    refresh_success_callback_cpp_t on_success_cb_;
    void RefreshTask();
};
#endif

#endif // _HTTP_REFRESH_H_