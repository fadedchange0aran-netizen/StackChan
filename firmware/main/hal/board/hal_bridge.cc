/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal_bridge.h"
#include "stackchan_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <application.h>
#include <board.h>
#include <display.h>
#include <mutex>
#include <assets.h>
#include <settings.h>

static const char* _tag = "HAL_BRIDGE";
static constexpr int64_t _xiaozhi_toggle_guard_us = 2000 * 1000LL;
static constexpr int64_t _xiaozhi_toggle_cooldown_us = 500 * 1000LL;

static constexpr std::string_view _xiaozhi_config_nvs_ns                           = "xiaozhi";
static constexpr std::string_view _xiaozhi_config_idle_shutdown_time_key           = "idle_shutdown";
static constexpr std::string_view _xiaozhi_config_allow_shutdown_when_charging_key = "shutdown_charge";

namespace hal_bridge {

/* -------------------------------------------------------------------------- */
/*                            State and touch point                           */
/* -------------------------------------------------------------------------- */

static std::mutex _mutex;
static Data_t _data;
static int64_t _xiaozhi_app_started_at_us = 0;
static int64_t _xiaozhi_last_toggle_at_us = 0;

void lock()
{
    _mutex.lock();
}

void unlock()
{
    _mutex.unlock();
}

Data_t& get_data()
{
    return _data;
}

void set_touch_point(int num, int x, int y)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _data.touchPoint.num = num;
    _data.touchPoint.x   = x;
    _data.touchPoint.y   = y;
}

TouchPoint_t get_touch_point()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.touchPoint;
}

bool is_xiaozhi_mode()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.isXiaozhiMode;
}

void set_xiaozhi_mode(bool mode)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _data.isXiaozhiMode              = mode;
    _data.isXiaozhiModeToggleEnabled = mode ? _data.isXiaozhiModeToggleEnabled : false;
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
#define DISPLAY_TYPE StackChanAvatarDisplay

lv_disp_t* display_get_lvgl_display()
{
    auto display = static_cast<DISPLAY_TYPE*>(Board::GetInstance().GetDisplay());
    return display->GetLvglDisplay();
}

void disply_lvgl_lock()
{
    auto display = static_cast<DISPLAY_TYPE*>(Board::GetInstance().GetDisplay());
    display->LvglLock();
}

void disply_lvgl_unlock()
{
    auto display = static_cast<DISPLAY_TYPE*>(Board::GetInstance().GetDisplay());
    display->LvglUnlock();
}

/* -------------------------------------------------------------------------- */
/*                                 Application                                */
/* -------------------------------------------------------------------------- */

void xiaozhi_board_init()
{
    // Init board
    auto& board = Board::GetInstance();
}

void start_xiaozhi_app()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _data.isXiaozhiMode              = true;
        _data.isXiaozhiModeToggleEnabled = false;
        _xiaozhi_app_started_at_us       = esp_timer_get_time();
    }

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}

XiaozhiConfig_t get_xiaozhi_config()
{
    XiaozhiConfig_t config;

    Settings settings(_xiaozhi_config_nvs_ns.data(), false);
    config.idleShutdownTimeSeconds = settings.GetInt(_xiaozhi_config_idle_shutdown_time_key.data(),
                                                     static_cast<int>(config.idleShutdownTimeSeconds));
    config.allowShutdownWhenCharging =
        settings.GetBool(_xiaozhi_config_allow_shutdown_when_charging_key.data(), config.allowShutdownWhenCharging);

    return config;
}

void set_xiaozhi_config(const XiaozhiConfig_t& config)
{
    Settings settings(_xiaozhi_config_nvs_ns.data(), true);
    settings.SetInt(_xiaozhi_config_idle_shutdown_time_key.data(), config.idleShutdownTimeSeconds);
    settings.SetBool(_xiaozhi_config_allow_shutdown_when_charging_key.data(), config.allowShutdownWhenCharging);
}

void app_play_sound(const std::string_view& sound)
{
    auto& app = Application::GetInstance();
    app.PlaySound(sound);
}

void toggle_xiaozhi_chat_state()
{
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    int64_t now_us = esp_timer_get_time();
    if (state == kDeviceStateStarting) {
        ESP_LOGI(_tag, "Ignore xiaozhi chat toggle while starting");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        const int touch_points = _data.touchPoint.num;
        if (!_data.isXiaozhiModeToggleEnabled) {
            const bool startup_guard_elapsed =
                _xiaozhi_app_started_at_us != 0 && (now_us - _xiaozhi_app_started_at_us) >= _xiaozhi_toggle_guard_us;
            const bool touch_released = touch_points == 0;
            if (!startup_guard_elapsed || !touch_released) {
                ESP_LOGI(_tag,
                         "Ignore xiaozhi chat toggle during startup guard (elapsed_ms=%ld, touch_points=%d)",
                         static_cast<long>((now_us - _xiaozhi_app_started_at_us) / 1000),
                         touch_points);
                return;
            }

            _data.isXiaozhiModeToggleEnabled = true;
            ESP_LOGI(_tag, "Xiaozhi chat toggle enabled after startup guard");
        }

        if (state == kDeviceStateConnecting) {
            ESP_LOGI(_tag,
                     "Ignore xiaozhi chat toggle while connecting (state=%d, touch_points=%d)",
                     static_cast<int>(state),
                     touch_points);
            return;
        }

        if (_xiaozhi_last_toggle_at_us != 0 &&
            (now_us - _xiaozhi_last_toggle_at_us) < _xiaozhi_toggle_cooldown_us) {
            ESP_LOGI(_tag,
                     "Ignore xiaozhi chat toggle during cooldown (delta_ms=%ld, state=%d, touch_points=%d)",
                     static_cast<long>((now_us - _xiaozhi_last_toggle_at_us) / 1000),
                     static_cast<int>(state),
                     touch_points);
            return;
        }

        _xiaozhi_last_toggle_at_us = now_us;
        ESP_LOGI(_tag,
                 "Forward xiaozhi chat toggle (state=%d, touch_points=%d, ts_us=%lld)",
                 static_cast<int>(state),
                 touch_points,
                 static_cast<long long>(now_us));
    }

    app.ToggleChatState();
}

}  // namespace hal_bridge
