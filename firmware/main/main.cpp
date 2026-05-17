/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <esp_system.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

const char* reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "unknown";
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_JTAG:      return "jtag";
        case ESP_RST_EFUSE:     return "efuse";
        case ESP_RST_PWR_GLITCH:return "power_glitch";
        case ESP_RST_CPU_LOCKUP:return "cpu_lockup";
        default:                return "unmapped";
    }
}

}

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);
    auto reset_reason = esp_reset_reason();
    mclog::tagInfo("BOOT", "reset reason: {} ({})", static_cast<int>(reset_reason),
                   reset_reason_to_string(reset_reason));

    // HAL init
    GetHAL().init();
    GetHAL().startVpsBridgeIfNeeded();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // In native mode, cold boot should enter the voice app directly.
    // Warm reboot still goes back to launcher so the home gesture keeps working.
    if (GetHAL().isNativeModePreferred() && GetHAL().getWarmRebootTarget() < 0) {
        GetHAL().startXiaozhi();
        return;
    }

    // Install apps
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppAiAgent>());
    GetMooncake().installApp(std::make_unique<AppAvatar>());
    GetMooncake().installApp(std::make_unique<AppEspnowControl>());
    GetMooncake().installApp(std::make_unique<AppAppCenter>());
    GetMooncake().installApp(std::make_unique<AppEzdata>());
    GetMooncake().installApp(std::make_unique<AppDance>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().updateHeapStatusLog();

        GetMooncake().update();

        if (GetHAL().isXiaozhiStartRequested()) {
            break;
        }
    }

    // Uninstall all apps and destroy mooncake
    GetMooncake().uninstallAllApps();
    DestroyMooncake();

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
