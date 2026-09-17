/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "brookesia/system_super/macro_configs.h"
#if !BROOKESIA_SYSTEM_SUPER_ENABLE_DEBUG_LOG
#   define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "boost/json.hpp"
#include "brookesia/service_helper/system/storage.hpp"
#include "private/font_language.hpp"
#include "private/shell_app.hpp"
#include "private/system_constants.hpp"
#include "private/utils.hpp"

namespace esp_brookesia::system::super {
namespace {

inline constexpr uint32_t STORAGE_FS_TIMEOUT_MS = 5000;

std::string normalize_theme_id(std::string_view theme_id)
{
    if (theme_id == "shell.light") {
        return SUPER_LIGHT_THEME_ID;
    }
    if (theme_id == "shell.dark") {
        return SUPER_DARK_THEME_ID;
    }
    return std::string(theme_id);
}

std::string get_current_language(core::AppContext *context)
{
    if (context == nullptr) {
        return {};
    }

    const auto supported_languages = context->gui().list_supported_languages();
    const auto language = context->gui().get_language();
    if (!language.empty() && contains_language(supported_languages, language)) {
        return language;
    }
    return select_default_font_language(context->gui());
}

std::string make_system_resource_dir(const System &system)
{
    return (std::filesystem::path(system.get_resource_root_path()) / "system" / "super").lexically_normal().generic_string();
}

std::string make_share_resource_dir(const System &system)
{
    return (std::filesystem::path(system.get_resource_root_path()) / "system").lexically_normal().generic_string();
}

std::string make_app_display_name(const core::AppInfo &app, core::AppContext *context)
{
    return core::resolve_app_display_name(app.manifest, get_current_language(context));
}

bool is_zh_cn_language(core::AppContext *context)
{
    return get_current_language(context) == "zh_CN";
}

std::string make_localized_page_title(ShellPage page, core::AppContext *context)
{
    if (!is_zh_cn_language(context)) {
        return to_page_title(page);
    }

    switch (page) {
    case ShellPage::Home:
        return "应用启动器";
    case ShellPage::AppLauncher:
        return "应用启动器";
    case ShellPage::Notifications:
        return "应用启动器";
    }
    return "应用启动器";
}

std::expected<std::vector<std::string>, std::string> list_theme_files(std::string_view resource_dir)
{
    const auto theme_dir = std::filesystem::path(resource_dir) / SUPER_THEME_DIR;
    auto dir_info = service::helper::Storage::fs_stat(theme_dir.generic_string(), STORAGE_FS_TIMEOUT_MS);
    if (!dir_info || !dir_info->exists || dir_info->type != service::helper::Storage::FileType::Directory) {
        return std::unexpected("Shell theme directory does not exist: " + theme_dir.string());
    }

    std::vector<std::string> files;
    auto entries = service::helper::Storage::fs_list(theme_dir.generic_string(), STORAGE_FS_TIMEOUT_MS);
    if (!entries) {
        return std::unexpected("Failed to scan Shell theme directory: " + theme_dir.string());
    }
    for (const auto &entry : entries.value()) {
        if (entry.info.type != service::helper::Storage::FileType::File) {
            continue;
        }
        const auto file_name = entry.name;
        if (std::filesystem::path(file_name).extension() == ".json" && file_name != "index.json") {
            files.push_back((std::filesystem::path(SUPER_THEME_DIR) / file_name).generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return std::unexpected("Shell theme directory contains no JSON themes: " + theme_dir.string());
    }
    return files;
}

std::vector<std::string> get_builtin_theme_files()
{
    return {
        (std::filesystem::path(SUPER_THEME_DIR) / "light.json").generic_string(),
        (std::filesystem::path(SUPER_THEME_DIR) / "dark.json").generic_string(),
    };
}

std::expected<std::vector<std::string>, std::string> list_font_files(std::string_view resource_dir)
{
    const auto font_dir = std::filesystem::path(resource_dir) / SUPER_FONT_DIR;
    auto dir_info = service::helper::Storage::fs_stat(font_dir.generic_string(), STORAGE_FS_TIMEOUT_MS);
    if (!dir_info || !dir_info->exists || dir_info->type != service::helper::Storage::FileType::Directory) {
        return std::unexpected("Shell font directory does not exist: " + font_dir.string());
    }

    std::vector<std::string> files;
    auto entries = service::helper::Storage::fs_list(font_dir.generic_string(), STORAGE_FS_TIMEOUT_MS);
    if (!entries) {
        return std::unexpected("Failed to scan Shell font directory: " + font_dir.string());
    }
    for (const auto &entry : entries.value()) {
        if (entry.info.type != service::helper::Storage::FileType::File) {
            continue;
        }
        if (std::filesystem::path(entry.name).extension() == ".json") {
            files.push_back((std::filesystem::path(SUPER_FONT_DIR) / entry.name).generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return std::unexpected("Shell font directory contains no JSON fonts: " + font_dir.string());
    }
    return files;
}

std::expected<std::string, std::string> read_text_file(const std::filesystem::path &path)
{
    auto result = service::helper::Storage::fs_read_text(path.generic_string(), STORAGE_FS_TIMEOUT_MS);
    if (!result) {
        return std::unexpected("Failed to read file: " + path.string() + ", error: " + result.error());
    }
    return result.value();
}

std::string json_value_to_string(const boost::json::value &value)
{
    if (value.is_string()) {
        return std::string(value.as_string());
    }
    return boost::json::serialize(value);
}

std::expected<std::vector<gui::BindingValueUpdate>, std::string> load_shell_i18n_updates(
    std::string_view resource_dir, std::string_view locale
)
{
    const auto locale_path = std::filesystem::path(std::string(resource_dir)) / "shell" / "i18n" /
                             (std::string(locale) + ".json");
    auto file_content = read_text_file(locale_path);
    if (!file_content) {
        return std::unexpected(file_content.error());
    }

    boost::system::error_code error_code;
    auto parsed = boost::json::parse(*file_content, error_code);
    if (error_code) {
        return std::unexpected(
                   "Failed to parse Shell i18n file '" + locale_path.string() + "': " + error_code.message()
               );
    }
    if (!parsed.is_object()) {
        return std::unexpected("Shell i18n file must be a JSON object: " + locale_path.string());
    }
    const auto &root = parsed.as_object();
    const auto *updates_value = root.if_contains("updates");
    if (updates_value == nullptr || !updates_value->is_array()) {
        return std::unexpected("Shell i18n file must contain an array field 'updates': " + locale_path.string());
    }

    std::vector<gui::BindingValueUpdate> updates;
    for (const auto &entry_value : updates_value->as_array()) {
        if (!entry_value.is_object()) {
            return std::unexpected("Shell i18n update entries must be objects: " + locale_path.string());
        }
        const auto &entry = entry_value.as_object();
        const auto *path_value = entry.if_contains("path");
        const auto *key_value = entry.if_contains("key");
        const auto *value = entry.if_contains("value");
        if (path_value == nullptr || key_value == nullptr || value == nullptr ||
                !path_value->is_string() || !key_value->is_string()) {
            return std::unexpected("Shell i18n update entries require string fields 'path' and 'key'");
        }
        updates.push_back(gui::BindingValueUpdate{
            .absolute_path = std::string(path_value->as_string()),
            .key = std::string(key_value->as_string()),
            .value = json_value_to_string(*value),
        });
    }
    return updates;
}

} // namespace

std::string ShellApp::get_current_page_title() const
{
    if (context_ == nullptr) {
        return make_localized_page_title(ShellPage::AppLauncher, context_);
    }

    auto state = context_->gui().get_screen_flow_state(SUPER_SHELL_PAGES_FLOW_ID);
    if (!state.has_value()) {
        return make_localized_page_title(ShellPage::AppLauncher, context_);
    }

    auto page = to_shell_page(*state);
    if (!page.has_value()) {
        BROOKESIA_LOGW("Unknown Shell screen flow state: %1%", *state);
        return make_localized_page_title(ShellPage::AppLauncher, context_);
    }
    return make_localized_page_title(*page, context_);
}

std::string ShellApp::get_app_display_name(const core::AppInfo &app) const
{
    return make_app_display_name(app, context_);
}

std::expected<void, std::string> ShellApp::apply_i18n_updates()
{
    if (context_ == nullptr) {
        return {};
    }
    const auto locale = get_current_language(context_);
    if (locale == applied_i18n_locale_) {
        return {};
    }

    const auto resource_dir = make_system_resource_dir(owner_);
    auto updates = load_shell_i18n_updates(resource_dir, locale);
    if (!updates) {
        const auto fallback_locale = select_default_font_language(context_->gui());
        if (!fallback_locale.empty() && locale != fallback_locale) {
            BROOKESIA_LOGW(
                "Failed to load Shell i18n for '%1%', falling back to '%2%': %3%",
                locale,
                fallback_locale,
                updates.error()
            );
            updates = load_shell_i18n_updates(resource_dir, fallback_locale);
        }
        if (!updates) {
            return std::unexpected(updates.error());
        }
    }

    auto result = context_->gui().set_binding_values(*updates);
    if (!result) {
        return result;
    }
    applied_i18n_locale_ = locale;
    return {};
}

std::expected<void, std::string> ShellApp::load_fonts(core::AppContext &context)
{
    if (owner_.shell_fonts_prepared_) {
        BROOKESIA_LOGI("Shell fonts already prepared by Super system; skip duplicate registration");
        return {};
    }

    const auto share_dir = make_share_resource_dir(owner_);
    auto font_files = list_font_files(share_dir);
    if (!font_files) {
        BROOKESIA_LOGI("Shell fonts skipped: %1%", font_files.error());
        return {};
    }

    for (const auto &relative_path : *font_files) {
        auto result = context.gui().register_font_file(share_dir, relative_path);
        if (!result) {
            return std::unexpected("Failed to register Shell font '" + relative_path + "': " + result.error());
        }
    }

    const auto supported_languages = context.gui().list_supported_languages();
    if (supported_languages.empty()) {
        BROOKESIA_LOGI("Shell font language fallback skipped: no registered font languages");
        return {};
    }

    const auto stored_language = owner_.get_stored_gui_language_preference();
    const std::string requested_language = (stored_language.has_value() && !stored_language->empty()) ?
                                           *stored_language :
                                           context.gui().get_language();
    auto fallback = apply_font_language_fallback(
                        context.gui(),
                        requested_language,
                        SUPER_DEFAULT_FONT_ID,
                        false
                    );
    if (!fallback) {
        return std::unexpected("Failed to apply Shell font language fallback: " + fallback.error());
    }
    BROOKESIA_LOGI(
        "Shell fonts registered: languages(%1%), language(%2%), default_font(%3%)",
        supported_languages,
        fallback->language,
        fallback->font_id
    );
    return {};
}

std::expected<void, std::string> ShellApp::apply_language_preference(core::AppContext &context)
{
    if (context.gui().list_supported_languages().empty()) {
        BROOKESIA_LOGI("System GUI language preference skipped: no registered font languages");
        return {};
    }

    auto stored_language = owner_.get_stored_gui_language_preference();
    const auto runtime_language_before = context.gui().get_language();
    const bool use_stored_language = stored_language.has_value() && !stored_language->empty();
    const std::string requested_language = use_stored_language ? *stored_language : runtime_language_before;
    auto fallback = apply_font_language_fallback(
                        context.gui(),
                        requested_language,
                        SUPER_DEFAULT_FONT_ID,
                        true
                    );
    if (!fallback) {
        return std::unexpected("Failed to apply Shell language font fallback: " + fallback.error());
    }

    if (use_stored_language && fallback->language == requested_language) {
        BROOKESIA_LOGI("System GUI language '%1%' is restored", fallback->language);
    } else if (use_stored_language) {
        BROOKESIA_LOGW(
            "Stored system GUI language '%1%' is not available; falling back to %2%",
            requested_language,
            fallback->language
        );
    } else if (fallback->language == runtime_language_before) {
        BROOKESIA_LOGI("System GUI language '%1%' is already active; skip duplicate reapply", fallback->language);
    } else {
        BROOKESIA_LOGD("Default system GUI language '%1%' is applied", fallback->language);
    }
    return {};
}

std::expected<void, std::string> ShellApp::load_themes(core::AppContext &context)
{
    if (owner_.shell_themes_prepared_) {
        auto stored_theme_id = owner_.get_stored_gui_theme_preference();
        current_theme_id_ = (stored_theme_id.has_value() && !stored_theme_id->empty()) ?
                            normalize_theme_id(*stored_theme_id) :
                            SUPER_DEFAULT_THEME_ID;
        BROOKESIA_LOGI("Shell themes already prepared by Super system; skip duplicate loading");
        return {};
    }

    const auto resource_dir = make_system_resource_dir(owner_);
    auto theme_files = list_theme_files(resource_dir);
    if (!theme_files) {
        theme_files = get_builtin_theme_files();
    }

    for (const auto &relative_path : *theme_files) {
        auto load_result = context.gui().load_theme_file(resource_dir, relative_path);
        if (!load_result) {
            return std::unexpected("Failed to load Shell theme '" + relative_path + "': " + load_result.error());
        }
    }

    auto stored_theme_id = owner_.get_stored_gui_theme_preference();
    const bool has_stored_theme = stored_theme_id.has_value() && !stored_theme_id->empty();
    bool use_stored_theme = has_stored_theme;
    current_theme_id_ = use_stored_theme ? normalize_theme_id(*stored_theme_id) : SUPER_DEFAULT_THEME_ID;
    auto set_result = context.gui().set_theme(current_theme_id_, true);
    if (!set_result && use_stored_theme) {
        BROOKESIA_LOGW(
            "Failed to restore system GUI theme '%1%': %2%; falling back to %3%",
            current_theme_id_,
            set_result.error(),
            SUPER_DEFAULT_THEME_ID
        );
        use_stored_theme = false;
        current_theme_id_ = SUPER_DEFAULT_THEME_ID;
        set_result = context.gui().set_theme(current_theme_id_, true);
    }
    if (!set_result) {
        return std::unexpected(
                   "Failed to set default Shell theme '" + current_theme_id_ + "': " +
                   set_result.error()
               );
    }
    if (use_stored_theme) {
        BROOKESIA_LOGI("System GUI theme '%1%' is restored", current_theme_id_);
    } else {
        BROOKESIA_LOGD("Default system GUI theme '%1%' is applied", current_theme_id_);
    }
    return {};
}

} // namespace esp_brookesia::system::super
