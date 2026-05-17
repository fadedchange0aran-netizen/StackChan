/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "secret_logic.h"
#include <settings.h>
#include "hal/private_config.h"
#include <esp_log.h>

namespace secret_logic {

static const std::string _setting_ns = "stackchan_custom";
static const std::string _setting_url_key = "server_url";
static const std::string _setting_key_key = "auth_key";
static const std::string _setting_use_custom_key = "use_custom";
static const std::string _setting_model_key = "model_name";

// 默认值
static const char* DEFAULT_CUSTOM_URL = PRIVATE_CUSTOM_URL;
static const char* DEFAULT_CUSTOM_KEY = PRIVATE_CUSTOM_KEY;
static const char* DEFAULT_MODEL_NAME = "gpt-3.5-turbo"; // 固件默认发送的模型名
static const char* ORIGINAL_DEFAULT_URL = "http://localhost:3000";
static const char* ORIGINAL_DEFAULT_KEY = "CHANGE_ME";

__attribute__((weak)) std::string get_server_url()
{
    Settings settings(_setting_ns, false);
    bool use_custom = settings.GetBool(_setting_use_custom_key, true); // 默认开启自定义
    
    if (use_custom) {
        return settings.GetString(_setting_url_key, DEFAULT_CUSTOM_URL);
    }
    return ORIGINAL_DEFAULT_URL;
}

__attribute__((weak)) std::string generate_auth_token()
{
    Settings settings(_setting_ns, false);
    bool use_custom = settings.GetBool(_setting_use_custom_key, true);
    
    if (use_custom) {
        return settings.GetString(_setting_key_key, DEFAULT_CUSTOM_KEY);
    }
    return ORIGINAL_DEFAULT_KEY;
}

// 获取模型名称
std::string get_model_name()
{
    Settings settings(_setting_ns, false);
    return settings.GetString(_setting_model_key, DEFAULT_MODEL_NAME);
}

// 供 WSS 指令调用，更新配置
void update_custom_config(const std::string& url, const std::string& key, const std::string& model, bool use_custom)
{
    Settings settings(_setting_ns, true);
    settings.SetBool(_setting_use_custom_key, use_custom);
    if (!url.empty()) settings.SetString(_setting_url_key, url);
    if (!key.empty()) settings.SetString(_setting_key_key, key);
    if (!model.empty()) settings.SetString(_setting_model_key, model);
}

__attribute__((weak)) std::string generate_handshake_token(std::string_view data)
{
    return PRIVATE_HANDSHAKE_TOKEN;
}

}  // namespace secret_logic
