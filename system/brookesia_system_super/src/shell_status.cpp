/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "brookesia/system_super/macro_configs.h"
#if !BROOKESIA_SYSTEM_SUPER_ENABLE_DEBUG_LOG
#   define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif

#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "brookesia/lib_utils/describe_helpers.hpp"
#include "brookesia/service_helper/network/sntp.hpp"
#include "brookesia/service_helper/network/wifi.hpp"
#include "brookesia/service_helper/system/device.hpp"
#include "private/shell_app.hpp"
#include "private/system_constants.hpp"
#include "private/utils.hpp"

namespace esp_brookesia::system::super {
namespace {

using SNTPHelper = service::helper::SNTP;
using WifiHelper = service::helper::Wifi;
using DeviceHelper = service::helper::Device;

struct WifiStatusState {
    bool visible = false;
    bool connected = false;
};

WifiStatusState get_wifi_status_from_state(std::string_view state)
{
    if (state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Connected)) {
        return {
            .visible = true,
            .connected = true,
        };
    }
    if (state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Started) ||
            state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Connecting) ||
            state == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralState::Disconnecting)) {
        return {
            .visible = true,
            .connected = false,
        };
    }
    return {};
}

WifiStatusState get_wifi_status_from_event(std::string_view event)
{
    if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Connected)) {
        return {
            .visible = true,
            .connected = true,
        };
    }
    if (event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Started) ||
            event == BROOKESIA_DESCRIBE_TO_STR(WifiHelper::GeneralEvent::Disconnected)) {
        return {
            .visible = true,
            .connected = false,
        };
    }
    return {};
}

std::string bool_to_binding(bool value)
{
    return value ? "true" : "false";
}

std::string make_clock_text()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    char buffer[8] = {};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M", &local_time) == 0) {
        return "--:--";
    }
    return buffer;
}

} // namespace

bool ShellApp::ensure_wifi_service_binding()
{
    if (wifi_service_binding_.is_valid()) {
        if (WifiHelper::is_running()) {
            return true;
        }
        ++wifi_status_generation_;
        set_status_wifi_state(false, false);
        return false;
    }
    if (!WifiHelper::is_available()) {
        ++wifi_status_generation_;
        set_status_wifi_state(false, false);
        return false;
    }
    wifi_service_binding_ = service::ServiceManager::get_instance().bind(WifiHelper::get_name().data());
    if (!wifi_service_binding_.is_valid()) {
        BROOKESIA_LOGW("Failed to bind Wi-Fi service for Shell status");
        ++wifi_status_generation_;
        set_status_wifi_state(false, false);
        return false;
    }
    if (!WifiHelper::is_running()) {
        ++wifi_status_generation_;
        wifi_service_binding_.release();
        set_status_wifi_state(false, false);
        return false;
    }

    if (!wifi_event_connection_.connected()) {
        const auto general_callback = [this](const std::string &, const std::string & event, bool unexpected) {
            if (unexpected) {
                BROOKESIA_LOGW("Wi-Fi service reported unexpected event for Shell status: %1%", event);
            }
            ++wifi_status_generation_;
            const auto status = get_wifi_status_from_event(event);
            set_status_wifi_state(status.visible, status.connected);
        };
        wifi_event_connection_ = WifiHelper::subscribe_event(
                                     WifiHelper::EventId::GeneralEventHappened,
                                     general_callback
                                 );
        if (!wifi_event_connection_.connected()) {
            BROOKESIA_LOGW("Failed to subscribe Wi-Fi general event for Shell status");
        }
    }
    return true;
}

void ShellApp::release_wifi_service_binding()
{
    ++wifi_status_generation_;
    wifi_event_connection_.disconnect();
    wifi_service_binding_.release();
    wifi_connected_ = false;
    set_status_wifi_state(false, false);
}

void ShellApp::refresh_wifi_status()
{
    if (!ensure_wifi_service_binding()) {
        return;
    }

    const auto generation = ++wifi_status_generation_;
    const auto state_handler = [this, generation](service::FunctionResult && result) {
        if (context_ == nullptr || generation != wifi_status_generation_ || !wifi_service_binding_.is_valid()) {
            return;
        }
        if (!result.success || !result.has_data()) {
            set_status_wifi_state(true, false);
            return;
        }
        const auto &state = result.get_data<std::string>();
        const auto status = get_wifi_status_from_state(state);
        set_status_wifi_state(status.visible, status.connected);
    };
    if (!WifiHelper::call_function_async(WifiHelper::FunctionId::GetGeneralState, state_handler)) {
        BROOKESIA_LOGW("Failed to submit Wi-Fi state request for Shell status");
        set_status_wifi_state(true, false);
    }
}

void ShellApp::set_status_wifi_state(bool visible, bool connected)
{
    wifi_connected_ = connected;
    if (context_ == nullptr) {
        return;
    }
    std::vector<gui::BindingValueUpdate> updates;
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_WIFI_PATH,
        .key = "wifi_hidden",
        .value = bool_to_binding(!visible),
    });
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = SUPER_STATUS_WIFI_PATH,
        .key = "wifi_bg",
        .value = connected ? "${color.success.fill}" : "${color.border.strong}",
    });
    updates.push_back(gui::BindingValueUpdate{
        .absolute_path = std::string(SUPER_STATUS_WIFI_PATH) + "/label",
        .key = "wifi_text",
        .value = connected ? "${color.success.on}" : "${color.text.inverse}",
    });
    auto result = context_->gui().set_binding_values(updates);
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell Wi-Fi status icon: %1%", result.error());
    }
}

bool ShellApp::ensure_device_service_binding()
{
    if (device_service_binding_.is_valid()) {
        return DeviceHelper::is_running();
    }
    if (!DeviceHelper::is_available()) {
        set_status_battery_state(std::nullopt, std::nullopt);
        return false;
    }
    device_service_binding_ = service::ServiceManager::get_instance().bind(DeviceHelper::get_name().data());
    if (!device_service_binding_.is_valid() || !DeviceHelper::is_running()) {
        device_service_binding_.release();
        set_status_battery_state(std::nullopt, std::nullopt);
        return false;
    }
    if (!battery_event_connection_.connected()) {
        auto callback = [this](const std::string &, const boost::json::object &data) {
            DeviceHelper::PowerBatteryState state;
            if (!BROOKESIA_DESCRIBE_FROM_JSON(data, state)) {
                BROOKESIA_LOGW("Failed to parse battery state for Shell status");
                return;
            }
            set_status_battery_state(
                state.is_present ? state.percentage : std::nullopt,
                state.is_present ? state.current_ma : std::nullopt
            );
        };
        battery_event_connection_ = DeviceHelper::subscribe_event(
                                        DeviceHelper::EventId::PowerBatteryStateChanged, callback
                                    );
    }
    return true;
}

void ShellApp::release_device_service_binding()
{
    battery_event_connection_.disconnect();
    device_service_binding_.release();
    set_status_battery_state(std::nullopt, std::nullopt);
}

void ShellApp::refresh_battery_status()
{
    if (!ensure_device_service_binding()) {
        return;
    }
    auto result = DeviceHelper::call_function_sync<boost::json::object>(
                      DeviceHelper::FunctionId::GetPowerBatteryState,
                      service::helper::Timeout(500)
                  );
    DeviceHelper::PowerBatteryState state;
    if (!result || !BROOKESIA_DESCRIBE_FROM_JSON(*result, state) || !state.is_present) {
        set_status_battery_state(std::nullopt, std::nullopt);
        return;
    }
    set_status_battery_state(state.percentage, state.current_ma);
}

void ShellApp::set_status_battery_state(
    const std::optional<uint8_t> &percentage, const std::optional<int32_t> &current_ma
)
{
    if (context_ == nullptr) {
        return;
    }
    const auto text = percentage.has_value() ? std::to_string(*percentage) + "%" : "--";
    std::vector<gui::BindingValueUpdate> updates;
    updates.push_back({SUPER_STATUS_BATTERY_PATH, "battery_hidden", bool_to_binding(!percentage.has_value())});
    const char *background = "${color.border.strong}";
    const char *foreground = "${color.text.inverse}";
    if (current_ma.value_or(0) > 5) {
        background = "${color.success.fill}";
        foreground = "${color.success.on}";
    } else if (current_ma.value_or(0) < -5) {
        background = "${color.danger.fill}";
        foreground = "${color.danger.on}";
    }
    updates.push_back({SUPER_STATUS_BATTERY_PATH, "battery_background", background});
    updates.push_back({std::string(SUPER_STATUS_BATTERY_PATH) + "/label", "battery_text", text});
    updates.push_back({std::string(SUPER_STATUS_BATTERY_PATH) + "/label", "battery_foreground", foreground});
    auto result = context_->gui().set_binding_values(updates);
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell battery status: %1%", result.error());
    }
}

bool ShellApp::ensure_sntp_service_binding()
{
    if (sntp_service_binding_.is_valid()) {
        return true;
    }
    if (!SNTPHelper::is_available()) {
        BROOKESIA_LOGD("SNTP service is not available for Shell status clock updates");
        return false;
    }

    sntp_service_binding_ = service::ServiceManager::get_instance().bind(SNTPHelper::get_name().data());
    if (!sntp_service_binding_.is_valid()) {
        BROOKESIA_LOGW("Failed to bind SNTP service for Shell status clock updates");
        return false;
    }
    return true;
}

void ShellApp::release_sntp_service_binding()
{
    disconnect_sntp_events();
    sntp_service_binding_.release();
}

void ShellApp::subscribe_sntp_events()
{
    disconnect_sntp_events();
    if (!ensure_sntp_service_binding()) {
        return;
    }

    const auto timezone_callback = [this](const std::string &, const service::EventItemMap &) {
        BROOKESIA_LOGI("SNTP timezone changed, refresh Shell status clock");
        refresh_status_clock();
        stop_status_clock_timer();
        schedule_status_clock_timer();
    };
    sntp_event_connection_ = SNTPHelper::subscribe_event(
                                 SNTPHelper::EventId::TimezoneChanged,
                                 timezone_callback
                             );
    if (!sntp_event_connection_.connected()) {
        BROOKESIA_LOGW("Failed to subscribe SNTP timezone event for Shell status clock");
    }
}

void ShellApp::disconnect_sntp_events()
{
    sntp_event_connection_.disconnect();
}

void ShellApp::refresh_status_clock()
{
    if (context_ == nullptr) {
        return;
    }
    auto result = context_->gui().set_binding_value(SUPER_STATUS_CLOCK_PATH, "clock_text", make_clock_text());
    if (!result) {
        BROOKESIA_LOGW("Failed to refresh Shell status clock: %1%", result.error());
    }
}

void ShellApp::schedule_status_clock_timer()
{
    if (context_ == nullptr || status_clock_timer_id_ != core::INVALID_TIMER_ID) {
        return;
    }
    auto timer = context_->timer().start_delayed(SUPER_STATUS_CLOCK_TIMER_NAME, 1000);
    if (!timer) {
        BROOKESIA_LOGW("Failed to start Shell status clock timer: %1%", timer.error());
        return;
    }
    status_clock_timer_id_ = *timer;
}

void ShellApp::stop_status_clock_timer()
{
    if (context_ != nullptr && status_clock_timer_id_ != core::INVALID_TIMER_ID) {
        (void)context_->timer().stop(status_clock_timer_id_);
    }
    status_clock_timer_id_ = core::INVALID_TIMER_ID;
}

} // namespace esp_brookesia::system::super
