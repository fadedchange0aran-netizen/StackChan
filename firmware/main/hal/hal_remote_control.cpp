/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <board.h>
#include <web_socket.h>
#include "hal/private_config.h"
#include <esp_log.h>
#include <display.h>
#include <wifi_manager.h>
#include <ArduinoJson.hpp>
#include <mbedtls/base64.h>
#include <image_to_jpeg.h>
#include <board.h>
#include "board/hal_bridge.h"
#include "board/stackchan_camera.h"
#include <settings.h>
#include <mutex>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <application.h>
#include <stackchan/modifiers/dance.h>
#include <stackchan/modifiers/timed.h>

static const char* _tag = "VPS-Bridge";
static const char* _voice_ws_setting_ns = "websocket";
static const char* _voice_ws_url_key = "url";
static const char* _voice_ws_token_key = "token";
static const char* _voice_ws_version_key = "version";

namespace {

struct PendingLauncherAction {
    char action[24];
    int value = 0;
};

QueueHandle_t g_launcher_action_queue = nullptr;
std::atomic<uint32_t> g_launcher_effect_generation{0};
std::atomic<bool> g_launcher_motion_task_running{false};
std::atomic<uint32_t> g_launcher_ui_hide_until{0};

struct LauncherNeutralResetContext {
    uint32_t generation = 0;
    uint32_t delay_ms = 0;
};

struct LauncherMotionContext {
    char action[24];
    uint32_t generation = 0;
};

class OptionalLvglLockGuard {
public:
    explicit OptionalLvglLockGuard(bool enabled) : enabled_(enabled)
    {
        if (enabled_) {
            GetHAL().lvglLock();
        }
    }

    ~OptionalLvglLockGuard()
    {
        if (enabled_) {
            GetHAL().lvglUnlock();
        }
    }

private:
    bool enabled_ = false;
};

bool RequiresLauncherMainLoop(const std::string& action_name)
{
    return action_name == "dance" || action_name == "shake_head" || action_name == "set_emotion";
}

bool IsDeadlineActive(uint32_t deadline, uint32_t now)
{
    return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
}

void EnsureLauncherActionQueue()
{
    if (g_launcher_action_queue == nullptr) {
        g_launcher_action_queue = xQueueCreate(8, sizeof(PendingLauncherAction));
    }
}

bool EnqueueLauncherAction(const std::string& action_name, int value)
{
    EnsureLauncherActionQueue();
    if (g_launcher_action_queue == nullptr) {
        return false;
    }

    PendingLauncherAction action{};
    strncpy(action.action, action_name.c_str(), sizeof(action.action) - 1);
    action.value = value;
    return xQueueSend(g_launcher_action_queue, &action, 0) == pdPASS;
}

void ApplyLauncherDisplayEmotion(const char* emotion_name)
{
    Board::GetInstance().GetDisplay()->SetEmotion(emotion_name);
}

void note_launcher_remote_effect_impl(uint32_t duration_ms)
{
    uint32_t now = GetHAL().millis();
    uint32_t requested_deadline = now + duration_ms;
    uint32_t current = g_launcher_ui_hide_until.load();
    while (true) {
        if (IsDeadlineActive(current, requested_deadline) || current == requested_deadline) {
            return;
        }
        if (g_launcher_ui_hide_until.compare_exchange_weak(current, requested_deadline)) {
            return;
        }
    }
}

bool should_hide_launcher_ui_impl()
{
    return IsDeadlineActive(g_launcher_ui_hide_until.load(), GetHAL().millis());
}

const char* EmotionNameFromIndex(int emotion)
{
    switch (emotion) {
        case 0: return "happy";
        case 1: return "sad";
        case 2: return "angry";
        case 3: return "surprised";
        case 4: return "sleepy";
        default: return "neutral";
    }
}

void UpdateRemoteTtsVisuals(const std::string& text, int emotion, bool launcher_mode)
{
    auto* display = Board::GetInstance().GetDisplay();
    display->SetEmotion(EmotionNameFromIndex(emotion));
    if (!text.empty()) {
        display->SetChatMessage("assistant", text.c_str());
    }
}

void ApplyLauncherRgb(uint8_t r, uint8_t g, uint8_t b)
{
    GetHAL().showRgbColor(r, g, b);
}

void ApplyLauncherVisibleEmotion(int emotion)
{
    auto& motion = GetStackChan().motion();

    switch (emotion) {
        case 0:  // happy
            ApplyLauncherRgb(120, 80, 0);
            motion.yawServo().moveWithSpeed(220, 220);
            motion.pitchServo().moveWithSpeed(-120, 220);
            break;
        case 1:  // sad
            ApplyLauncherRgb(0, 0, 100);
            motion.yawServo().moveWithSpeed(-180, 180);
            motion.pitchServo().moveWithSpeed(180, 180);
            break;
        case 2:  // angry
            ApplyLauncherRgb(140, 0, 0);
            motion.yawServo().moveWithSpeed(260, 240);
            motion.pitchServo().moveWithSpeed(120, 240);
            break;
        case 3:  // surprised
            ApplyLauncherRgb(90, 90, 90);
            motion.yawServo().moveWithSpeed(0, 260);
            motion.pitchServo().moveWithSpeed(-220, 260);
            break;
        case 4:  // sleepy
            ApplyLauncherRgb(0, 40, 90);
            motion.yawServo().moveWithSpeed(-120, 160);
            motion.pitchServo().moveWithSpeed(260, 160);
            break;
        default:
            ApplyLauncherRgb(0, 0, 0);
            motion.goHome(220);
            break;
    }
}

void LauncherNeutralResetTask(void* param)
{
    std::unique_ptr<LauncherNeutralResetContext> ctx(static_cast<LauncherNeutralResetContext*>(param));
    vTaskDelay(pdMS_TO_TICKS(ctx->delay_ms));
    if (ctx->generation == g_launcher_effect_generation.load()) {
        ApplyLauncherDisplayEmotion("neutral");
        ApplyLauncherRgb(0, 0, 0);
        GetStackChan().motion().goHome(220);
    }
    vTaskDelete(nullptr);
}

void ScheduleLauncherNeutralReset(uint32_t generation, uint32_t delay_ms)
{
    note_launcher_remote_effect_impl(delay_ms);
    auto* ctx = new LauncherNeutralResetContext{generation, delay_ms};
    if (xTaskCreate(&LauncherNeutralResetTask, "launcher_neutral", 3072, ctx, 1, nullptr) != pdPASS) {
        delete ctx;
        ESP_LOGW(_tag, "Failed to create launcher neutral reset task");
    }
}

void LauncherMotionTask(void* param)
{
    std::unique_ptr<LauncherMotionContext> ctx(static_cast<LauncherMotionContext*>(param));
    auto& motion = GetStackChan().motion();
    std::string action(ctx->action);

    if (action == "shake_head") {
        ApplyLauncherDisplayEmotion("surprised");
        ApplyLauncherRgb(120, 120, 120);
        motion.pitchServo().moveWithSpeed(-140, 220);
        motion.yawServo().moveWithSpeed(420, 260);
        vTaskDelay(pdMS_TO_TICKS(320));
        ApplyLauncherRgb(120, 0, 120);
        motion.yawServo().moveWithSpeed(-420, 260);
        vTaskDelay(pdMS_TO_TICKS(360));
        ApplyLauncherRgb(0, 0, 120);
        motion.yawServo().moveWithSpeed(360, 260);
        vTaskDelay(pdMS_TO_TICKS(320));
        motion.goHome(220);
        ScheduleLauncherNeutralReset(ctx->generation, 1200);
    } else if (action == "dance") {
        ApplyLauncherDisplayEmotion("happy");
        ApplyLauncherRgb(120, 40, 0);
        motion.yawServo().moveWithSpeed(380, 260);
        motion.pitchServo().moveWithSpeed(-180, 260);
        vTaskDelay(pdMS_TO_TICKS(320));
        ApplyLauncherRgb(0, 120, 40);
        motion.yawServo().moveWithSpeed(-380, 260);
        motion.pitchServo().moveWithSpeed(180, 260);
        vTaskDelay(pdMS_TO_TICKS(360));
        ApplyLauncherRgb(120, 0, 80);
        motion.yawServo().moveWithSpeed(320, 240);
        motion.pitchServo().moveWithSpeed(-120, 240);
        vTaskDelay(pdMS_TO_TICKS(320));
        ApplyLauncherRgb(0, 80, 120);
        motion.yawServo().moveWithSpeed(-320, 240);
        motion.pitchServo().moveWithSpeed(120, 240);
        vTaskDelay(pdMS_TO_TICKS(340));
        motion.goHome(220);
        ScheduleLauncherNeutralReset(ctx->generation, 1600);
    }

    g_launcher_motion_task_running = false;
    vTaskDelete(nullptr);
}

void StartLauncherMotionSequence(const std::string& action_name)
{
    if (g_launcher_motion_task_running.exchange(true)) {
        ESP_LOGW(_tag, "Launcher motion already running, ignore action=%s", action_name.c_str());
        return;
    }

    uint32_t generation = ++g_launcher_effect_generation;
    note_launcher_remote_effect_impl(action_name == "dance" ? 2200 : 1800);
    auto* ctx = new LauncherMotionContext{};
    strncpy(ctx->action, action_name.c_str(), sizeof(ctx->action) - 1);
    ctx->generation = generation;
    if (xTaskCreate(&LauncherMotionTask, "launcher_motion", 4096, ctx, 2, nullptr) != pdPASS) {
        g_launcher_motion_task_running = false;
        delete ctx;
        ESP_LOGW(_tag, "Failed to create launcher motion task for %s", action_name.c_str());
    }
}

}  // namespace

void note_launcher_remote_effect(uint32_t duration_ms)
{
    note_launcher_remote_effect_impl(duration_ms);
}

bool should_hide_launcher_ui()
{
    return should_hide_launcher_ui_impl();
}

static stackchan::avatar::Emotion EmotionFromIndex(int emotion) {
    switch (emotion) {
        case 0: return stackchan::avatar::Emotion::Happy;
        case 1: return stackchan::avatar::Emotion::Sad;
        case 2: return stackchan::avatar::Emotion::Angry;
        case 3: return stackchan::avatar::Emotion::Doubt;
        case 4: return stackchan::avatar::Emotion::Sleepy;
        default: return stackchan::avatar::Emotion::Neutral;
    }
}

static void ApplyHardwareAction(const std::string& action_name, int value) {
    auto& stackchan = GetStackChan();
    bool launcher_mode = !GetHAL().isXiaozhiRunning();
    const bool needs_lvgl_lock = !launcher_mode;

    if (action_name == "rotate_head" || action_name == "look_left" || action_name == "look_right") {
        int yaw = value;
        if (action_name == "look_left") {
            yaw = 600;
        } else if (action_name == "look_right") {
            yaw = -600;
        }
        OptionalLvglLockGuard lock(needs_lvgl_lock);
        stackchan.motion().yawServo().moveWithSpeed(yaw, 220);
    } else if (action_name == "nod_head" || action_name == "look_up" || action_name == "look_down") {
        int pitch = value;
        if (action_name == "look_up") {
            pitch = -280;
        } else if (action_name == "look_down") {
            pitch = 320;
        }
        OptionalLvglLockGuard lock(needs_lvgl_lock);
        stackchan.motion().pitchServo().moveWithSpeed(pitch, 220);
    } else if (action_name == "center_head" || action_name == "look_center") {
        OptionalLvglLockGuard lock(needs_lvgl_lock);
        stackchan.motion().goHome(220);
    } else if (action_name == "shake_head") {
        if (launcher_mode) {
            StartLauncherMotionSequence(action_name);
        } else {
            LvglLockGuard lock;
            stackchan.addModifier(std::make_unique<stackchan::DanceModifier>(stackchan::DanceModifier::Panic));
        }
    } else if (action_name == "dance") {
        if (launcher_mode) {
            StartLauncherMotionSequence(action_name);
        } else {
            LvglLockGuard lock;
            stackchan.addModifier(std::make_unique<stackchan::DanceModifier>(stackchan::DanceModifier::Happy));
        }
    } else if (action_name == "set_emotion") {
        if (launcher_mode) {
            uint32_t generation = ++g_launcher_effect_generation;
            ApplyLauncherDisplayEmotion(EmotionNameFromIndex(value));
            ApplyLauncherVisibleEmotion(value);
            ScheduleLauncherNeutralReset(generation, 2500);
        } else {
            Board::GetInstance().GetDisplay()->SetEmotion(EmotionNameFromIndex(value));
            LvglLockGuard lock;
            stackchan.addModifier(std::make_unique<stackchan::TimedEmotionModifier>(EmotionFromIndex(value), 3000));
        }
    } else if (action_name == "reboot") {
        GetHAL().reboot();
    }
}

void pump_pending_launcher_actions()
{
    if (g_launcher_action_queue == nullptr || GetHAL().isXiaozhiRunning()) {
        return;
    }

    PendingLauncherAction action{};
    if (xQueueReceive(g_launcher_action_queue, &action, 0) != pdPASS) {
        return;
    }

    const std::string action_name(action.action);
    if (RequiresLauncherMainLoop(action_name) && g_launcher_motion_task_running.load()) {
        if (xQueueSendToFront(g_launcher_action_queue, &action, 0) != pdPASS) {
            ESP_LOGW(_tag, "Failed to requeue launcher action while motion is running: %s", action.action);
        }
        return;
    }

    ESP_LOGI(_tag, "Applying queued launcher action: %s, value=%d", action.action, action.value);
    ApplyHardwareAction(action_name, action.value);
}

// Forward declaration
namespace secret_logic {
    void update_custom_config(const std::string& url, const std::string& key, const std::string& model, bool use_custom);
}

class VPSBridgeClient {
public:
    void init(const std::string& vps_url) {
        _vps_url = vps_url;
        _deviceId = GetHAL().getFactoryMacString();
        connect();
    }

    void connect() {
        _last_reconnect_attempt = GetHAL().millis();

        if (!WifiManager::GetInstance().IsConnected()) {
            ESP_LOGW(_tag, "WiFi not connected, waiting...");
            return;
        }

        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        if (!network) {
            ESP_LOGW(_tag, "Network not ready, waiting...");
            return;
        }

        // Recreate the websocket for each reconnect attempt so stale internal
        // state from a previous disconnection does not accumulate.
        auto websocket = network->CreateWebSocket(5);
        if (!websocket) {
            ESP_LOGE(_tag, "Failed to create websocket");
            return;
        }

        websocket->OnConnected([this]() {
            ESP_LOGI(_tag, "Connected to VPS Bridge!");
            
            // Register
            ArduinoJson::JsonDocument doc;
            doc["type"] = "register";
            doc["id"] = _deviceId;
            doc["token"] = PRIVATE_BRIDGE_TOKEN;
            std::string out;
            ArduinoJson::serializeJson(doc, out);
            
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            if (_websocket) {
                _websocket->Send(out.c_str());
            }
            
            _last_heartbeat_time = GetHAL().millis();
        });

        websocket->OnDisconnected([this]() {
            ESP_LOGI(_tag, "Disconnected from VPS Bridge");
        });

        websocket->OnData([this](const char* data, size_t len, bool binary) {
            if (!binary) {
                handleJsonMessage(data, len);
            }
        });

        {
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            _websocket = std::move(websocket);
        }

        // 再次确认 _websocket 依然有效
        if (_websocket) {
            if (!_websocket->Connect(_vps_url.c_str())) {
                ESP_LOGE(_tag, "Connect to VPS failed");
            }
        }
    }

    void update() {
        if (!_websocket || !_websocket->IsConnected()) {
            if (GetHAL().millis() - _last_reconnect_attempt > 5000) {
                connect();
            }
        } else {
            // Heartbeat
            if (GetHAL().millis() - _last_heartbeat_time > 15000) {
                ESP_LOGI(_tag, "Sending ping...");
                std::lock_guard<std::mutex> lock(_websocket_mutex);
                if (_websocket) {
                    _websocket->Send("{\"type\":\"ping\"}");
                }
                _last_heartbeat_time = GetHAL().millis();
            }
        }
    }

private:
    std::string BuildVisionExplainUrl() const {
        std::string url = _vps_url;
        if (url.rfind("wss://", 0) == 0) {
            url.replace(0, 6, "https://");
        } else if (url.rfind("ws://", 0) == 0) {
            url.replace(0, 5, "http://");
        }

        auto scheme_pos = url.find("://");
        auto path_pos = url.find('/', scheme_pos == std::string::npos ? 0 : scheme_pos + 3);
        if (path_pos != std::string::npos) {
            url.resize(path_pos);
        }
        url += "/vision/explain";
        return url;
    }

    std::string LoadBridgeToken() const {
        Settings ws_settings(_voice_ws_setting_ns, false);
        auto token = ws_settings.GetString(_voice_ws_token_key);
        if (!token.empty()) {
            return token;
        }
        return PRIVATE_BRIDGE_TOKEN;
    }

    void handleJsonMessage(const char* data, size_t len) {
        // 使用 cJSON 处理大数据包，因为它对 PSRAM 更友好
        cJSON* root = cJSON_Parse(data);
        if (!root) return;

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        const char* type = type_item ? type_item->valuestring : nullptr;

        if (type && strcmp(type, "pong") == 0) {
            _last_heartbeat_time = GetHAL().millis();
        } else if (type && strcmp(type, "tts") == 0) {
            cJSON* text_item = cJSON_GetObjectItem(root, "text");
            cJSON* audio_item = cJSON_GetObjectItem(root, "audio");
            cJSON* emotion_item = cJSON_GetObjectItem(root, "emotion");

            const char* text = (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : "";
            const char* audio_b64 = (audio_item && cJSON_IsString(audio_item)) ? audio_item->valuestring : "";
            int emotion = (emotion_item && cJSON_IsNumber(emotion_item)) ? emotion_item->valueint : 5;
            std::string text_str = text;
            
            ESP_LOGI(_tag, "Remote TTS received: %s (Audio len: %d)", text, (int)strlen(audio_b64));
            bool launcher_mode = !GetHAL().isXiaozhiRunning();
            
            // 1. 显示文字并设置表情
            UpdateRemoteTtsVisuals(text_str, emotion, launcher_mode);
            
            if (launcher_mode) {
                uint32_t generation = ++g_launcher_effect_generation;
                ScheduleLauncherNeutralReset(generation, 2500);
                if (strlen(audio_b64) > 0) {
                    ESP_LOGI(_tag, "Skip remote TTS audio playback in launcher mode");
                }
            }
            
            // 2. 播放音频流
            if (!launcher_mode && strlen(audio_b64) > 0) {
                uint32_t tts_generation = ++_tts_generation;
                size_t decoded_len = 0;
                if (mbedtls_base64_decode(nullptr, 0, &decoded_len,
                                          reinterpret_cast<const unsigned char*>(audio_b64),
                                          strlen(audio_b64)) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
                    std::string audio_data(decoded_len, '\0');
                    size_t output_len = 0;
                    int ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(audio_data.data()),
                                                    audio_data.size(),
                                                    &output_len,
                                                    reinterpret_cast<const unsigned char*>(audio_b64),
                                                    strlen(audio_b64));
                    if (ret == 0 && output_len > 0) {
                        audio_data.resize(output_len);
                        hal_bridge::app_play_sound(std::string_view(audio_data.data(), audio_data.size()));
                        ScheduleNeutralAfterPlayback(tts_generation);
                    } else {
                        ESP_LOGE(_tag, "Failed to decode remote TTS audio, code=%d", ret);
                    }
                } else {
                    ESP_LOGE(_tag, "Failed to get decoded TTS audio length");
                }
            }
        } else if (type && strcmp(type, "capture_photo") == 0) {
            ESP_LOGI(_tag, "Remote capture_photo request received");
            if (_photo_task_running.exchange(true)) {
                ESP_LOGW(_tag, "Photo capture already in progress, ignoring duplicate request");
            } else {
                auto* ctx = new PhotoCaptureContext{this};
                if (xTaskCreatePinnedToCoreWithCaps(&VPSBridgeClient::CapturePhotoTask, "robot_photo", 1024 * 12, ctx, 3,
                                                    nullptr, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
                    delete ctx;
                    _photo_task_running = false;
                    ESP_LOGE(_tag, "Failed to create photo capture task");
                }
            }
        } else if (type && strcmp(type, "interpret_photo") == 0) {
            cJSON* prompt_item = cJSON_GetObjectItem(root, "prompt");
            const char* prompt = (prompt_item && cJSON_IsString(prompt_item)) ? prompt_item->valuestring : "请详细描述你看到的内容。";
            ESP_LOGI(_tag, "Remote interpret_photo request received");
            if (_photo_task_running.exchange(true)) {
                ESP_LOGW(_tag, "Photo task already in progress, ignoring interpret request");
            } else {
                auto* ctx = new PhotoInterpretContext{this, std::string(prompt)};
                if (xTaskCreatePinnedToCoreWithCaps(&VPSBridgeClient::InterpretPhotoTask, "robot_vision", 1024 * 14, ctx, 3,
                                                    nullptr, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
                    delete ctx;
                    _photo_task_running = false;
                    ESP_LOGE(_tag, "Failed to create interpret photo task");
                }
            }
        } else if (type && strcmp(type, "hw_control") == 0) {
            cJSON* action_item = cJSON_GetObjectItem(root, "action");
            cJSON* value_item = cJSON_GetObjectItem(root, "value");

            const char* action = (action_item && cJSON_IsString(action_item)) ? action_item->valuestring : "";
            int value = (value_item && cJSON_IsNumber(value_item)) ? value_item->valueint : 0;
            
            ESP_LOGI(_tag, "Action: %s, Value: %d", action, value);

            auto action_name = std::string(action);
            if (GetHAL().isXiaozhiRunning()) {
                Application::GetInstance().Schedule([action_name, value]() {
                    ApplyHardwareAction(action_name, value);
                });
            } else if (RequiresLauncherMainLoop(action_name)) {
                if (EnqueueLauncherAction(action_name, value)) {
                    ESP_LOGI(_tag, "Queued launcher action: %s, value=%d", action, value);
                } else {
                    ESP_LOGE(_tag, "Failed to queue launcher action: %s", action);
                }
            } else {
                ESP_LOGI(_tag, "Applying action immediately in launcher mode");
                ApplyHardwareAction(action_name, value);
            }
        } else if (type && strcmp(type, "set_mode") == 0) {
            cJSON* xiaozhi_item = cJSON_GetObjectItem(root, "xiaozhi");
            bool xiaozhi = false;
            if (xiaozhi_item) {
                if (cJSON_IsBool(xiaozhi_item)) xiaozhi = cJSON_IsTrue(xiaozhi_item);
                else if (cJSON_IsNumber(xiaozhi_item)) xiaozhi = (xiaozhi_item->valueint != 0);
            }
            ESP_LOGI(_tag, "Switching mode: xiaozhi=%d", xiaozhi);
            
            if (!xiaozhi) {
                // 1. 设置 WebSocket 参数 (官方协议使用的命名空间)
                Settings ws_settings("websocket", true);
                ws_settings.SetString("url", PRIVATE_XIAOZHI_WS_URL);
                ws_settings.SetString("token", PRIVATE_BRIDGE_TOKEN);
                ws_settings.SetInt("version", 1);
                
                // 2. 清空 MQTT 终端，强制系统选择 WebSocket 协议
                Settings mqtt_settings("mqtt", true);
                mqtt_settings.SetString("endpoint", "");
                
                // 3. 修改 OTA URL 防止重启后被官方服务器强制激活并覆盖配置
                Settings wifi_settings("wifi", true);
                wifi_settings.SetString("ota_url", "http://127.0.0.1/dummy"); 

                // 4. 确保 AI Agent 应用认为已经配置完成
                Settings app_settings("app_config", true);
                app_settings.SetBool("is_configed", true);
            } else {
                // 切回官方模式：恢复默认配置，让系统重新通过 OTA 激活获取官方参数
                Settings wifi_settings("wifi", true);
                wifi_settings.SetString("ota_url", ""); // 恢复为空，系统将使用默认的 CONFIG_OTA_URL
                
                Settings mqtt_settings("mqtt", true);
                mqtt_settings.SetString("endpoint", "");
                
                Settings ws_settings("websocket", true);
                ws_settings.SetString("url", "");
                ws_settings.SetString("token", "");
                
                // 标记为未配置，这样启动时可能会重新进入激活流程
                Settings app_settings("app_config", true);
                app_settings.SetBool("is_configed", false);
            }
            
            GetHAL().setNativeModePreferred(!xiaozhi);
            hal_bridge::set_xiaozhi_mode(xiaozhi);
            
            // 必须重启才能让 Application 类重新读取协议配置
            GetHAL().reboot();
        }
        cJSON_Delete(root);
    }

    struct EmotionResetContext {
        VPSBridgeClient* client;
        uint32_t generation;
    };

    struct PhotoCaptureContext {
        VPSBridgeClient* client;
    };

    struct PhotoInterpretContext {
        VPSBridgeClient* client;
        std::string prompt;
    };

    static void CapturePhotoTask(void* param) {
        std::unique_ptr<PhotoCaptureContext> ctx(static_cast<PhotoCaptureContext*>(param));
        ctx->client->CaptureAndSendPhoto();
        ctx->client->_photo_task_running = false;
        vTaskDelete(nullptr);
    }

    static void InterpretPhotoTask(void* param) {
        std::unique_ptr<PhotoInterpretContext> ctx(static_cast<PhotoInterpretContext*>(param));
        ctx->client->CaptureAndInterpretPhoto(ctx->prompt);
        ctx->client->_photo_task_running = false;
        vTaskDelete(nullptr);
    }

    static void RestoreNeutralTask(void* param) {
        std::unique_ptr<EmotionResetContext> ctx(static_cast<EmotionResetContext*>(param));
        if (!GetHAL().isXiaozhiRunning()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            ctx->client->ScheduleNeutralIfLatest(ctx->generation);
            vTaskDelete(nullptr);
            return;
        }

        auto& app = Application::GetInstance();
        int idle_checks = 0;

        while (true) {
            if (ctx->generation != ctx->client->_tts_generation.load()) {
                vTaskDelete(nullptr);
                return;
            }

            if (app.GetAudioService().IsIdle()) {
                idle_checks++;
                if (idle_checks >= 3) {
                    ctx->client->ScheduleNeutralIfLatest(ctx->generation);
                    vTaskDelete(nullptr);
                    return;
                }
            } else {
                idle_checks = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }

    void ScheduleNeutralAfterPlayback(uint32_t generation) {
        auto* ctx = new EmotionResetContext{this, generation};
        if (xTaskCreate(&VPSBridgeClient::RestoreNeutralTask, "tts_neutral", 4096, ctx, 1, nullptr) != pdPASS) {
            delete ctx;
            ESP_LOGW(_tag, "Failed to create TTS neutral restore task");
        }
    }

    void ScheduleNeutralIfLatest(uint32_t generation) {
        if (GetHAL().isXiaozhiRunning()) {
            Application::GetInstance().Schedule([this, generation]() {
                if (generation != _tts_generation.load()) {
                    return;
                }
                Board::GetInstance().GetDisplay()->SetEmotion("neutral");
            });
        } else {
            if (generation != _tts_generation.load()) {
                return;
            }
            Board::GetInstance().GetDisplay()->SetEmotion("neutral");
        }
    }

    void CaptureAndSendPhoto() {
        auto camera = static_cast<StackChanCamera*>(hal_bridge::board_get_camera());
        if (!camera) {
            ESP_LOGE(_tag, "Camera not available");
            return;
        }
        if (!camera->Capture()) {
            ESP_LOGE(_tag, "Camera capture failed");
            return;
        }

        uint8_t* jpeg_buf = nullptr;
        size_t jpeg_len = 0;
        if (!image_to_jpeg((uint8_t*)camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                           camera->GetFrameHeight(), (v4l2_pix_fmt_t)camera->GetFrameFormat(), 25, &jpeg_buf,
                           &jpeg_len)) {
            ESP_LOGE(_tag, "JPEG encode failed");
            return;
        }

        if (!jpeg_buf || jpeg_len == 0) {
            ESP_LOGE(_tag, "JPEG buffer empty");
            free(jpeg_buf);
            return;
        }

        if (!_websocket || !_websocket->IsConnected()) {
            ESP_LOGW(_tag, "Control websocket disconnected before photo upload");
            free(jpeg_buf);
            return;
        }

        static constexpr size_t kRawChunkBytes = 3072;  // multiple of 3, so intermediate base64 chunks have no padding
        static constexpr size_t kMaxChunkB64Bytes = ((kRawChunkBytes + 2) / 3) * 4;
        const size_t total_chunks = (jpeg_len + kRawChunkBytes - 1) / kRawChunkBytes;

        std::string begin_msg = "{\"type\":\"photo_begin\",\"size\":";
        begin_msg += std::to_string(jpeg_len);
        begin_msg += ",\"total_chunks\":";
        begin_msg += std::to_string(total_chunks);
        begin_msg += "}";

        {
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            if (_websocket && _websocket->IsConnected()) {
                _websocket->Send(begin_msg.c_str());
            }
        }

        char* b64_buf = static_cast<char*>(heap_caps_malloc(kMaxChunkB64Bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!b64_buf) {
            ESP_LOGE(_tag, "Failed to allocate photo chunk buffer");
            free(jpeg_buf);
            return;
        }

        size_t sent_chunks = 0;
        for (size_t offset = 0; offset < jpeg_len; offset += kRawChunkBytes) {
            const size_t remaining = jpeg_len - offset;
            const size_t raw_len = remaining < kRawChunkBytes ? remaining : kRawChunkBytes;
            size_t encoded_len = 0;
            int encode_ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64_buf), kMaxChunkB64Bytes + 1,
                                                   &encoded_len, jpeg_buf + offset, raw_len);
            if (encode_ret != 0) {
                ESP_LOGE(_tag, "Base64 encode failed at chunk=%u code=%d", static_cast<unsigned>(sent_chunks), encode_ret);
                heap_caps_free(b64_buf);
                free(jpeg_buf);
                return;
            }
            b64_buf[encoded_len] = '\0';

            std::string chunk_msg = "{\"type\":\"photo_chunk\",\"index\":";
            chunk_msg += std::to_string(sent_chunks);
            chunk_msg += ",\"data\":\"";
            chunk_msg.append(b64_buf, encoded_len);
            chunk_msg += "\"}";

            std::lock_guard<std::mutex> lock(_websocket_mutex);
            if (!_websocket || !_websocket->IsConnected()) {
                ESP_LOGW(_tag, "Control websocket disconnected during photo upload");
                heap_caps_free(b64_buf);
                free(jpeg_buf);
                return;
            }
            _websocket->Send(chunk_msg.c_str());
            sent_chunks++;
        }

        heap_caps_free(b64_buf);
        free(jpeg_buf);

        {
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            if (_websocket && _websocket->IsConnected()) {
                _websocket->Send("{\"type\":\"photo_end\"}");
            }
        }
        ESP_LOGI(_tag, "Photo upload completed jpeg=%u chunks=%u", static_cast<unsigned>(jpeg_len),
                 static_cast<unsigned>(sent_chunks));
    }

    void CaptureAndInterpretPhoto(const std::string& prompt) {
        auto camera = static_cast<StackChanCamera*>(hal_bridge::board_get_camera());
        if (!camera) {
            ESP_LOGE(_tag, "Camera not available for interpret");
            SendInterpretResult("相机不可用。", false);
            return;
        }

        camera->SetExplainUrl(BuildVisionExplainUrl(), LoadBridgeToken());
        TaskPriorityReset priority_reset(1);

        try {
            if (!camera->Capture()) {
                ESP_LOGE(_tag, "Camera capture failed for interpret");
                SendInterpretResult("拍照失败。", false);
                return;
            }
            std::string result = camera->Explain(prompt);
            ESP_LOGI(_tag, "Interpret photo done, text_len=%u", static_cast<unsigned>(result.size()));
            SendInterpretResult(result, true);
        } catch (const std::exception& exc) {
            ESP_LOGE(_tag, "Interpret photo failed: %s", exc.what());
            SendInterpretResult(std::string("视觉解读失败: ") + exc.what(), false);
        } catch (...) {
            ESP_LOGE(_tag, "Interpret photo failed with unknown error");
            SendInterpretResult("视觉解读失败: 未知错误", false);
        }
    }

    void SendInterpretResult(const std::string& text, bool ok) {
        ArduinoJson::JsonDocument doc;
        doc["type"] = "photo_interpret_result";
        doc["ok"] = ok;
        doc["text"] = text;
        std::string out;
        ArduinoJson::serializeJson(doc, out);

        std::lock_guard<std::mutex> lock(_websocket_mutex);
        if (_websocket && _websocket->IsConnected()) {
            _websocket->Send(out.c_str());
        }
    }

    std::unique_ptr<WebSocket> _websocket;
    std::mutex _websocket_mutex;
    std::string _vps_url;
    std::string _deviceId;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_heartbeat_time = 0;
    std::atomic<uint32_t> _tts_generation{0};
    std::atomic<bool> _photo_task_running{false};
};

static std::unique_ptr<VPSBridgeClient> _bridge_client;

void vps_bridge_task(void* param) {
    _bridge_client = std::make_unique<VPSBridgeClient>();
    // 通过 Cloudflare Tunnel 走标准 WSS (443) 端口
    _bridge_client->init(PRIVATE_BRIDGE_URL);

    while (true) {
        _bridge_client->update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
