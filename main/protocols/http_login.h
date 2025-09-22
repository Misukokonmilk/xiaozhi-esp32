#ifndef _HTTP_LOGIN_H_
#define _HTTP_LOGIN_H_

#include <functional>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

// Callback function type for login success
typedef void (*login_success_callback_t)(const char *token);

/**
 * @brief Start the login task to authenticate with the server and get a token
 * 
 * @param on_success_cb Callback function to be called when login is successful
 */
void http_login_start_task(login_success_callback_t on_success_cb);

#ifdef __cplusplus
}

#include <functional>
// C++风格的回调类型
typedef std::function<void(const std::string& token)> login_success_callback_cpp_t;

// C++类接口
class HttpLogin {
public:
    HttpLogin();
    ~HttpLogin();
    
    // 启动登录任务
    void StartTask(login_success_callback_cpp_t on_success_cb);
    
    // 设置服务器配置
    void SetServerConfig(const std::string& host, int port, const std::string& endpoint);
    void SetCredentials(const std::string& username, const std::string& password);
    
private:
    std::string server_host_;
    int server_port_;
    std::string login_endpoint_;
    std::string login_username_;
    std::string login_password_;
    std::string token_json_field_;
    
    login_success_callback_cpp_t on_success_cb_;
    
    // 登录任务函数
    void LoginTask();
};
#endif

#endif // _HTTP_LOGIN_H_