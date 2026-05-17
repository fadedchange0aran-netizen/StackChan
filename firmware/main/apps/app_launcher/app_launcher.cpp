/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <board.h>
#include <display.h>
#include <hal/hal_remote_control.h>
#include <wifi_manager.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>

using namespace mooncake;

namespace {

std::atomic<bool> g_launcher_network_start_requested = false;

void launcher_network_task(void* param)
{
    (void)param;
    GetHAL().startNetwork({});
    g_launcher_network_start_requested = false;
    mclog::tagInfo("LAUNCHER", "background launcher network ready");
    vTaskDelete(nullptr);
}

void ensure_launcher_network_started(std::string_view tag)
{
    if (!GetHAL().isAppConfiged() || WifiManager::GetInstance().IsConnected()) {
        return;
    }

    bool expected = false;
    if (!g_launcher_network_start_requested.compare_exchange_strong(expected, true)) {
        return;
    }

    mclog::tagInfo(tag, "starting launcher network in background, native_preferred={}, warm_reboot_target={}",
                   GetHAL().isNativeModePreferred(), GetHAL().getWarmRebootTarget());
    if (xTaskCreate(launcher_network_task, "launcher_net", 6144, nullptr, 4, nullptr) != pdPASS) {
        g_launcher_network_start_requested = false;
        mclog::tagError(tag, "failed to start launcher network task");
    }
}

}  // namespace

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_config_checked && !GetHAL().isAppConfiged()) {
        mclog::tagInfo(getAppInfo().name, "app not configured, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
        ensure_launcher_network_started(getAppInfo().name);
    }
}

void AppLauncher::onLauncherRunning()
{
    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_config_checked = true;
            create_launcher_view();
            ensure_launcher_network_started(getAppInfo().name);
        }
    } else {
        ensure_launcher_network_started(getAppInfo().name);
        _view->update();
        screensaver_update();

        // 只有在 WiFi 已连接且 10 秒没有操作的情况下，才自动进入 AI 模式
        // 增加更严格的安全性检查，防止在启动初期崩溃
        if (!_auto_ai_agent_attempted && !GetHAL().isNativeModePreferred() && WifiManager::GetInstance().IsConnected() && 
            lv_display_get_inactive_time(NULL) > 10000 && 
            (GetHAL().millis() - _auto_ai_agent_tick > 10000)) {
            
            mclog::tagInfo(getAppInfo().name, "WiFi ready and idle, attempting auto open AI Agent");
            
            auto apps = getAppProps();
            for (const auto& app : apps) {
                if (app.info.name == "AI.AGENT") {
                    mclog::tagInfo(getAppInfo().name, "Auto opening AI.AGENT (ID: {})", app.appID);
                    _auto_ai_agent_attempted = true;
                    openApp(app.appID);
                    break;
                }
            }
            _auto_ai_agent_tick = GetHAL().millis();
        }
    }

    bool hide_launcher_ui = should_hide_launcher_ui();
    if (_view) {
        _view->setHidden(hide_launcher_ui);
    }

    pump_pending_launcher_actions();
    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppLauncher::create_launcher_view()
{
    // 确保底层 Avatar UI 已初始化，否则隐藏 Launcher Panel 时会看到白屏
    Board::GetInstance().GetDisplay()->SetupUI();

    _view = std::make_unique<view::LauncherView>();
    _view->init(getAppProps());
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        openApp(appID);
    };
    
    // 重置自动进入 AI 模式的计时器
    _auto_ai_agent_tick = GetHAL().millis();
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;
    if (should_hide_launcher_ui()) {
        if (_screensaver) {
            _screensaver.reset();
        }
        return;
    }

    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver) {
            _screensaver = std::make_unique<view::Screensaver>();
            _screensaver->init();
        }
    } else if (_screensaver) {
        _screensaver.reset();
    }

    // Update in 30ms interval
    if (_screensaver && GetHAL().millis() - _screensaver_timecount > 30) {
        _screensaver_timecount = GetHAL().millis();
        _screensaver->update();
    }
}
