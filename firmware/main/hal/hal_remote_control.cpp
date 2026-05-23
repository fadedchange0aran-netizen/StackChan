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
#include <esp_heap_caps.h>
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

constexpr uint32_t kPhotoCapturePowerSettleMs = 180;
constexpr size_t kPhotoUploadRawChunkSize = 2048;

bool EncodePhotoChunkToBase64(const uint8_t* data, size_t len, std::string& out)
{
    size_t encoded_len = 0;
    int probe_ret = mbedtls_base64_encode(nullptr, 0, &encoded_len, data, len);
    if (probe_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_len == 0) {
        return false;
    }

    out.resize(encoded_len);
    int encode_ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]),
                                           out.size(),
                                           &encoded_len,
                                           data,
                                           len);
    if (encode_ret != 0 || encoded_len == 0) {
        return false;
    }
    out.resize(encoded_len);
    return true;
}

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

class AtomicBoolResetGuard {
public:
    explicit AtomicBoolResetGuard(std::atomic<bool>& flag) : flag_(flag) {}

    ~AtomicBoolResetGuard()
    {
        flag_ = false;
    }

private:
    std::atomic<bool>& flag_;
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
        EnsurePhotoWorkerStarted();
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

        {
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            _websocket.reset();
        }

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

        if (_websocket) {
            if (!_websocket->Connect(_vps_url.c_str())) {
                ESP_LOGE(_tag, "Connect to VPS failed");
                std::lock_guard<std::mutex> lock(_websocket_mutex);
                _websocket.reset();
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
    enum class PhotoJobType {
        Capture,
        Interpret,
    };

    struct PhotoJob {
        PhotoJobType type = PhotoJobType::Capture;
        std::string prompt;
        std::string url;
        std::string token;
    };

    static constexpr uint32_t kPhotoWorkerStackSize = 6144;
    static constexpr uint32_t kPhotoWorkerQueueDepth = 1;

    static const char* PhotoJobTypeName(PhotoJobType type) {
        switch (type) {
            case PhotoJobType::Capture:
                return "capture";
            case PhotoJobType::Interpret:
                return "interpret";
            default:
                return "unknown";
        }
    }

    void EnsurePhotoWorkerStarted() {
        if (_photo_worker_handle != nullptr && _photo_job_queue != nullptr) {
            return;
        }

        if (_photo_job_queue == nullptr) {
            _photo_job_queue = xQueueCreate(kPhotoWorkerQueueDepth, sizeof(PhotoJob*));
            if (_photo_job_queue == nullptr) {
                ESP_LOGE(_tag, "Failed to create photo worker queue");
                return;
            }
        }

        if (_photo_worker_handle == nullptr) {
            if (xTaskCreate(&VPSBridgeClient::PhotoWorkerTask,
                            "photo_worker",
                            kPhotoWorkerStackSize,
                            this,
                            2,
                            &_photo_worker_handle) != pdPASS) {
                ESP_LOGE(_tag, "Failed to create photo worker task");
                vQueueDelete(_photo_job_queue);
                _photo_job_queue = nullptr;
                return;
            }
            ESP_LOGI(_tag, "Photo worker started stack=%u queue=%u",
                     static_cast<unsigned>(kPhotoWorkerStackSize),
                     static_cast<unsigned>(kPhotoWorkerQueueDepth));
        }
    }

    bool EnqueuePhotoJob(std::unique_ptr<PhotoJob> job) {
        EnsurePhotoWorkerStarted();
        if (_photo_job_queue == nullptr || _photo_worker_handle == nullptr || !job) {
            return false;
        }

        PhotoJob* raw_job = job.release();
        if (xQueueSend(_photo_job_queue, &raw_job, 0) != pdPASS) {
            delete raw_job;
            ESP_LOGW(_tag, "Photo worker queue is full");
            return false;
        }
        return true;
    }

    static void PhotoWorkerTask(void* param) {
        auto* client = static_cast<VPSBridgeClient*>(param);
        client->RunPhotoWorker();
    }

    void RunPhotoWorker() {
        while (true) {
            PhotoJob* raw_job = nullptr;
            if (xQueueReceive(_photo_job_queue, &raw_job, portMAX_DELAY) != pdPASS) {
                continue;
            }
            if (raw_job == nullptr) {
                continue;
            }

            std::unique_ptr<PhotoJob> job(raw_job);
            AtomicBoolResetGuard photo_task_guard(_photo_task_running);
            ESP_LOGI(_tag, "Photo worker begin type=%s", PhotoJobTypeName(job->type));

            if (job->type == PhotoJobType::Capture) {
                CaptureAndSendPhoto(job->url, job->token);
            } else {
                CaptureAndInterpretPhoto(job->prompt, job->url, job->token);
            }

            const UBaseType_t stack_hwm_words = uxTaskGetStackHighWaterMark(nullptr);
            ESP_LOGI(_tag, "Photo worker done type=%s stack_hwm=%u words (%u bytes)",
                     PhotoJobTypeName(job->type),
                     static_cast<unsigned>(stack_hwm_words),
                     static_cast<unsigned>(stack_hwm_words * sizeof(StackType_t)));
        }
    }

    std::string BuildBridgeHttpUrl(const char* path) const {
        if (path == nullptr || path[0] == '\0') {
            return {};
        }
        if (strncmp(path, "https://", 8) == 0 || strncmp(path, "http://", 7) == 0) {
            return std::string(path);
        }

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
        url += path;
        return url;
    }

    std::string BuildVisionExplainUrl() const {
        return BuildBridgeHttpUrl("/vision/explain");
    }

    std::string BuildPhotoCaptureUrl() const {
        return BuildBridgeHttpUrl("/capture");
    }

    std::string LoadBridgeToken() const {
        Settings ws_settings(_voice_ws_setting_ns, false);
        auto token = ws_settings.GetString(_voice_ws_token_key);
        if (!token.empty()) {
            return token;
        }
        return PRIVATE_BRIDGE_TOKEN;
    }

    void StartRemoteTtsStream(const std::string& text, int emotion, int sample_rate, int frame_duration, int packet_count) {
        ESP_LOGI(_tag, "Remote TTS stream start: text_len=%u packets=%d sample_rate=%d frame_ms=%d",
                 static_cast<unsigned>(text.size()), packet_count, sample_rate, frame_duration);
        bool launcher_mode = !GetHAL().isXiaozhiRunning();
        UpdateRemoteTtsVisuals(text, emotion, launcher_mode);

        if (launcher_mode) {
            _tts_stream_active = false;
            uint32_t generation = ++g_launcher_effect_generation;
            ScheduleLauncherNeutralReset(generation, 2500);
            if (packet_count > 0) {
                ESP_LOGW(_tag, "Skip remote TTS stream playback in launcher mode");
            }
            return;
        }

        _tts_generation++;
        _tts_stream_active = true;
        _tts_stream_sample_rate = sample_rate > 0 ? sample_rate : 24000;
        _tts_stream_frame_duration = frame_duration > 0 ? frame_duration : 60;
        Application::GetInstance().GetAudioService().ResetDecoder();
    }

    void AbortRemoteTtsStream(const char* reason) {
        const uint32_t generation = _tts_generation.load();
        if (reason != nullptr && reason[0] != '\0') {
            ESP_LOGW(_tag, "Abort remote TTS stream: %s", reason);
        }
        _tts_stream_active = false;
        Application::GetInstance().GetAudioService().ResetDecoder();
        ScheduleNeutralIfLatest(generation);
    }

    void AppendRemoteTtsStreamPacket(const char* packet_b64) {
        if (!_tts_stream_active || packet_b64 == nullptr || packet_b64[0] == '\0') {
            return;
        }

        size_t decoded_len = 0;
        int probe_ret = mbedtls_base64_decode(nullptr, 0, &decoded_len,
                                              reinterpret_cast<const unsigned char*>(packet_b64),
                                              strlen(packet_b64));
        if (probe_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_len == 0) {
            ESP_LOGE(_tag, "Failed to get decoded TTS stream packet length, code=%d", probe_ret);
            AbortRemoteTtsStream("invalid base64 packet length");
            return;
        }

        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = _tts_stream_sample_rate;
        packet->frame_duration = _tts_stream_frame_duration;
        packet->payload.resize(decoded_len);

        size_t output_len = 0;
        int decode_ret = mbedtls_base64_decode(packet->payload.data(),
                                               packet->payload.size(),
                                               &output_len,
                                               reinterpret_cast<const unsigned char*>(packet_b64),
                                               strlen(packet_b64));
        if (decode_ret != 0 || output_len == 0) {
            ESP_LOGE(_tag, "Failed to decode TTS stream packet, code=%d", decode_ret);
            AbortRemoteTtsStream("base64 decode failed");
            return;
        }

        packet->payload.resize(output_len);
        if (!Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(std::move(packet), true)) {
            ESP_LOGE(_tag, "Failed to queue TTS stream packet for playback");
            AbortRemoteTtsStream("decoder queue push failed");
        }
    }

    void FinishRemoteTtsStream() {
        if (!_tts_stream_active) {
            return;
        }
        _tts_stream_active = false;
        ScheduleNeutralAfterPlayback(_tts_generation.load());
    }

    void StopRemoteTtsPlayback() {
        ESP_LOGI(_tag, "Stopping remote TTS playback");
        _tts_stream_active = false;
        uint32_t generation = ++_tts_generation;

        if (GetHAL().isXiaozhiRunning()) {
            Application::GetInstance().GetAudioService().ResetDecoder();
            ScheduleNeutralIfLatest(generation);
            return;
        }

        uint32_t launcher_generation = ++g_launcher_effect_generation;
        ApplyLauncherDisplayEmotion("neutral");
        ApplyLauncherRgb(0, 0, 0);
        GetStackChan().motion().goHome(220);
        g_launcher_ui_hide_until = 0;
        ScheduleLauncherNeutralReset(launcher_generation, 100);
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
            cJSON* audio_path_item = cJSON_GetObjectItem(root, "audio_path");
            cJSON* emotion_item = cJSON_GetObjectItem(root, "emotion");

            const char* text = (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : "";
            const char* audio_path = (audio_path_item && cJSON_IsString(audio_path_item)) ? audio_path_item->valuestring : "";
            bool has_legacy_audio_b64 =
                audio_item && cJSON_IsString(audio_item) && audio_item->valuestring != nullptr &&
                audio_item->valuestring[0] != '\0';
            int emotion = (emotion_item && cJSON_IsNumber(emotion_item)) ? emotion_item->valueint : 5;
            std::string text_str = text;

            ESP_LOGI(_tag, "Remote TTS received: %s (audio_path_len=%d, legacy_audio_b64=%d)",
                     text, static_cast<int>(strlen(audio_path)), has_legacy_audio_b64 ? 1 : 0);
            if (has_legacy_audio_b64) {
                ESP_LOGW(_tag, "Ignoring deprecated remote TTS audio payload; use tts_stream_*");
            }
            if (strlen(audio_path) > 0) {
                ESP_LOGW(_tag, "Ignoring deprecated remote TTS audio_path payload; use tts_stream_*");
            }

            bool launcher_mode = !GetHAL().isXiaozhiRunning();

            // 1. 显示文字并设置表情
            UpdateRemoteTtsVisuals(text_str, emotion, launcher_mode);

            if (launcher_mode) {
                uint32_t generation = ++g_launcher_effect_generation;
                ScheduleLauncherNeutralReset(generation, 2500);
            }
        } else if (type && strcmp(type, "tts_stream_start") == 0) {
            cJSON* text_item = cJSON_GetObjectItem(root, "text");
            cJSON* emotion_item = cJSON_GetObjectItem(root, "emotion");
            cJSON* sample_rate_item = cJSON_GetObjectItem(root, "sample_rate");
            cJSON* frame_duration_item = cJSON_GetObjectItem(root, "frame_duration");
            cJSON* packet_count_item = cJSON_GetObjectItem(root, "packet_count");

            const char* text = (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : "";
            int emotion = (emotion_item && cJSON_IsNumber(emotion_item)) ? emotion_item->valueint : 5;
            int sample_rate = (sample_rate_item && cJSON_IsNumber(sample_rate_item)) ? sample_rate_item->valueint : 24000;
            int frame_duration = (frame_duration_item && cJSON_IsNumber(frame_duration_item)) ? frame_duration_item->valueint : 60;
            int packet_count = (packet_count_item && cJSON_IsNumber(packet_count_item)) ? packet_count_item->valueint : 0;

            StartRemoteTtsStream(text, emotion, sample_rate, frame_duration, packet_count);
        } else if (type && strcmp(type, "tts_stream_chunk") == 0) {
            cJSON* data_item = cJSON_GetObjectItem(root, "data");
            const char* packet_b64 = (data_item && cJSON_IsString(data_item)) ? data_item->valuestring : "";
            AppendRemoteTtsStreamPacket(packet_b64);
        } else if (type && strcmp(type, "tts_stream_end") == 0) {
            FinishRemoteTtsStream();
        } else if (type && strcmp(type, "stop_tts") == 0) {
            StopRemoteTtsPlayback();
        } else if (type && strcmp(type, "capture_photo") == 0) {
            ESP_LOGI(_tag, "Remote capture_photo request received");
            if (_photo_task_running.exchange(true)) {
                ESP_LOGW(_tag, "Photo capture already in progress, ignoring duplicate request");
                SendCaptureResult("已有拍照任务在执行，请稍后重试。", false);
            } else {
                auto job = std::make_unique<PhotoJob>();
                job->type = PhotoJobType::Capture;
                job->url = BuildPhotoCaptureUrl();
                job->token = LoadBridgeToken();
                if (!EnqueuePhotoJob(std::move(job))) {
                    _photo_task_running = false;
                    SendCaptureResult("拍照 worker 不可用或队列已满。", false);
                }
            }
        } else if (type && strcmp(type, "interpret_photo") == 0) {
            cJSON* prompt_item = cJSON_GetObjectItem(root, "prompt");
            const char* prompt = (prompt_item && cJSON_IsString(prompt_item)) ? prompt_item->valuestring : "请详细描述你看到的内容。";
            ESP_LOGI(_tag, "Remote interpret_photo request received");
            if (_photo_task_running.exchange(true)) {
                ESP_LOGW(_tag, "Photo task already in progress, ignoring interpret request");
                SendInterpretResult("已有拍照任务在执行，请稍后重试。", false);
            } else {
                auto job = std::make_unique<PhotoJob>();
                job->type = PhotoJobType::Interpret;
                job->prompt = prompt;
                job->url = BuildVisionExplainUrl();
                job->token = LoadBridgeToken();
                if (!EnqueuePhotoJob(std::move(job))) {
                    _photo_task_running = false;
                    SendInterpretResult("拍照 worker 不可用或队列已满。", false);
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
                Settings ws_settings(_voice_ws_setting_ns, true);
                ws_settings.SetString(_voice_ws_url_key, PRIVATE_XIAOZHI_WS_URL);
                ws_settings.SetString(_voice_ws_token_key, PRIVATE_BRIDGE_TOKEN);
                ws_settings.SetInt(_voice_ws_version_key, 1);
                
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
                
                Settings ws_settings(_voice_ws_setting_ns, true);
                ws_settings.SetString(_voice_ws_url_key, "");
                ws_settings.SetString(_voice_ws_token_key, "");
                
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

    struct PhotoCapturePowerState {
        uint8_t previous_speaker_volume = 0;
        bool muted_speaker = false;
    };

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

    bool PrepareForPhotoCapture(PhotoCapturePowerState& state) {
        StopRemoteTtsPlayback();

        state = {};
        state.previous_speaker_volume = GetHAL().getSpeakerVolume();
        if (state.previous_speaker_volume > 0) {
            GetHAL().setSpeakerVolume(0, false);
            state.muted_speaker = true;
            ESP_LOGI(_tag, "Photo capture prep: mute speaker %u -> 0",
                     static_cast<unsigned>(state.previous_speaker_volume));
        }

        vTaskDelay(pdMS_TO_TICKS(kPhotoCapturePowerSettleMs));
        return true;
    }

    void RestoreAfterPhotoCapture(const PhotoCapturePowerState& state) {
        if (state.muted_speaker) {
            GetHAL().setSpeakerVolume(state.previous_speaker_volume, false);
        }
    }

    void CaptureAndSendPhoto(const std::string& capture_url, const std::string& bridge_token) {
        (void)capture_url;
        (void)bridge_token;
        TaskPriorityReset priority_reset(1);
        auto camera = static_cast<StackChanCamera*>(hal_bridge::board_get_camera());
        if (!camera) {
            ESP_LOGE(_tag, "Camera not available");
            SendCaptureResult("相机不可用。", false);
            return;
        }

        camera->SetExplainUrl(capture_url, bridge_token);
        PhotoCapturePowerState power_state{};
        if (!PrepareForPhotoCapture(power_state)) {
            SendCaptureResult("拍照准备失败。", false);
            return;
        }

        try {
            if (!camera->Capture()) {
                ESP_LOGE(_tag, "Camera capture failed");
                SendCaptureResult("拍照失败。", false);
            } else if (!StreamCapturedPhotoOverWebSocket(*camera)) {
                ESP_LOGE(_tag, "Photo websocket upload failed");
                SendCaptureResult("拍照失败: 通过桥接 WebSocket 回传图片失败。", false);
            } else {
                ESP_LOGI(_tag, "Photo uploaded through bridge websocket");
            }
        } catch (const std::exception& exc) {
            ESP_LOGE(_tag, "Photo capture/upload failed: %s", exc.what());
            SendCaptureResult(std::string("拍照失败: ") + exc.what(), false);
        } catch (...) {
            ESP_LOGE(_tag, "Photo capture/upload failed with unknown error");
            SendCaptureResult("拍照失败: 未知错误", false);
        }
        RestoreAfterPhotoCapture(power_state);
    }

    bool StreamCapturedPhotoOverWebSocket(StackChanCamera& camera) {
        uint8_t* frame_data = const_cast<uint8_t*>(camera.GetFrameData());
        size_t frame_size = camera.GetFrameSize();
        uint16_t frame_width = static_cast<uint16_t>(camera.GetFrameWidth());
        uint16_t frame_height = static_cast<uint16_t>(camera.GetFrameHeight());
        v4l2_pix_fmt_t frame_format = static_cast<v4l2_pix_fmt_t>(camera.GetFrameFormat());

        if (frame_data == nullptr || frame_size == 0 || frame_width == 0 || frame_height == 0 || frame_format == 0) {
            ESP_LOGE(_tag, "Invalid frame for websocket upload");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(_websocket_mutex);
            if (!_websocket || !_websocket->IsConnected()) {
                ESP_LOGE(_tag, "Bridge websocket is not connected");
                return false;
            }
        }

        ArduinoJson::JsonDocument begin_doc;
        begin_doc["type"] = "photo_begin";
        begin_doc["size"] = 0;
        begin_doc["total_chunks"] = 0;
        if (!SendWebSocketJson(begin_doc)) {
            return false;
        }

        struct UploadContext {
            VPSBridgeClient* client;
            bool failed = false;
            size_t raw_bytes = 0;
            size_t chunk_count = 0;
        } ctx{this};

        bool jpeg_ok = image_to_jpeg_cb(
            frame_data,
            frame_size,
            frame_width,
            frame_height,
            frame_format,
            60,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto* ctx = static_cast<UploadContext*>(arg);
                if (ctx->failed || data == nullptr || len == 0) {
                    return 0;
                }

                const uint8_t* bytes = static_cast<const uint8_t*>(data);
                size_t offset = 0;
                while (offset < len) {
                    size_t part_len = std::min(kPhotoUploadRawChunkSize, len - offset);
                    std::string b64_chunk;
                    if (!EncodePhotoChunkToBase64(bytes + offset, part_len, b64_chunk)) {
                        ESP_LOGE(_tag, "Failed to base64 encode JPEG chunk at index=%u", static_cast<unsigned>(index));
                        ctx->failed = true;
                        return offset;
                    }

                    ArduinoJson::JsonDocument chunk_doc;
                    chunk_doc["type"] = "photo_chunk";
                    chunk_doc["index"] = ctx->chunk_count;
                    chunk_doc["data"] = b64_chunk;
                    if (!ctx->client->SendWebSocketJson(chunk_doc)) {
                        ESP_LOGE(_tag, "Failed to send photo websocket chunk=%u", static_cast<unsigned>(ctx->chunk_count));
                        ctx->failed = true;
                        return offset;
                    }

                    ctx->raw_bytes += part_len;
                    ctx->chunk_count++;
                    offset += part_len;
                }
                return len;
            },
            &ctx);

        if (!jpeg_ok || ctx.failed || ctx.chunk_count == 0) {
            ESP_LOGE(_tag, "JPEG encode or websocket upload failed raw=%u chunks=%u ok=%d failed=%d",
                     static_cast<unsigned>(ctx.raw_bytes),
                     static_cast<unsigned>(ctx.chunk_count),
                     jpeg_ok ? 1 : 0,
                     ctx.failed ? 1 : 0);
            return false;
        }

        ArduinoJson::JsonDocument end_doc;
        end_doc["type"] = "photo_end";
        end_doc["total_chunks"] = ctx.chunk_count;
        end_doc["size"] = ctx.raw_bytes;
        if (!SendWebSocketJson(end_doc)) {
            return false;
        }

        ESP_LOGI(_tag, "Photo websocket upload complete raw=%u chunks=%u",
                 static_cast<unsigned>(ctx.raw_bytes),
                 static_cast<unsigned>(ctx.chunk_count));
        return true;
    }

    void CaptureAndInterpretPhoto(const std::string& prompt, const std::string& explain_url, const std::string& bridge_token) {
        (void)prompt;
        (void)explain_url;
        (void)bridge_token;
        auto camera = static_cast<StackChanCamera*>(hal_bridge::board_get_camera());
        if (!camera) {
            ESP_LOGE(_tag, "Camera not available for interpret");
            SendInterpretResult("相机不可用。", false);
            return;
        }

        TaskPriorityReset priority_reset(1);
        PhotoCapturePowerState power_state{};
        if (!PrepareForPhotoCapture(power_state)) {
            SendInterpretResult("拍照准备失败。", false);
            return;
        }

        try {
            if (!camera->Capture()) {
                ESP_LOGE(_tag, "Camera capture failed for interpret");
                SendInterpretResult("拍照失败。", false);
            } else if (!StreamCapturedPhotoOverWebSocket(*camera)) {
                ESP_LOGE(_tag, "Photo websocket upload failed for interpret");
                SendInterpretResult("视觉解读失败: 通过桥接 WebSocket 回传图片失败。", false);
            } else {
                ESP_LOGI(_tag, "Interpret photo image uploaded through bridge websocket");
            }
        } catch (const std::exception& exc) {
            ESP_LOGE(_tag, "Interpret photo failed: %s", exc.what());
            SendInterpretResult(std::string("视觉解读失败: ") + exc.what(), false);
        } catch (...) {
            ESP_LOGE(_tag, "Interpret photo failed with unknown error");
            SendInterpretResult("视觉解读失败: 未知错误", false);
        }
        RestoreAfterPhotoCapture(power_state);
    }

    void SendInterpretResult(const std::string& text, bool ok) {
        ArduinoJson::JsonDocument doc;
        doc["type"] = "photo_interpret_result";
        doc["ok"] = ok;
        doc["text"] = text;
        SendWebSocketJson(doc);
    }

    void SendCaptureResult(const std::string& value, bool ok) {
        ArduinoJson::JsonDocument doc;
        doc["type"] = "photo_capture_result";
        doc["ok"] = ok;
        if (ok) {
            doc["path"] = value;
        } else {
            doc["message"] = value;
        }

        SendWebSocketJson(doc);
    }

    bool SendWebSocketJson(const ArduinoJson::JsonDocument& doc) {
        std::string out;
        ArduinoJson::serializeJson(doc, out);

        std::lock_guard<std::mutex> lock(_websocket_mutex);
        if (_websocket && _websocket->IsConnected()) {
            _websocket->Send(out.c_str());
            return true;
        }
        return false;
    }

    std::unique_ptr<WebSocket> _websocket;
    std::mutex _websocket_mutex;
    QueueHandle_t _photo_job_queue = nullptr;
    TaskHandle_t _photo_worker_handle = nullptr;
    std::string _vps_url;
    std::string _deviceId;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_heartbeat_time = 0;
    std::atomic<uint32_t> _tts_generation{0};
    int _tts_stream_sample_rate = 24000;
    int _tts_stream_frame_duration = 60;
    bool _tts_stream_active = false;
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
