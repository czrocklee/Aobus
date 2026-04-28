// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/AppConfig.h"
#include "core/Log.h"

#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace
{
  constexpr auto kAppGroup = "app";
  constexpr auto kWindowGroup = "window";
  constexpr auto kTrackViewGroup = "track_view";

  constexpr auto kConfigVersionKey = "config_version";
  constexpr auto kLastLibraryKey = "last_library";
  constexpr auto kLastBackendKey = "last_backend";
  constexpr auto kLastDeviceIdKey = "last_device_id";
  constexpr auto kWidthKey = "width";
  constexpr auto kHeightKey = "height";
  constexpr auto kMaximizedKey = "maximized";
  constexpr auto kPanedPositionKey = "paned_position";
  constexpr auto kColumnOrderKey = "column_order";
  constexpr auto kHiddenColumnsKey = "hidden_columns";
  constexpr auto kColumnWidthsKey = "column_widths";

  constexpr std::int32_t kAppConfigVersion = 1;

  std::filesystem::path configPath()
  {
    return std::filesystem::path{Glib::get_user_config_dir()} / "rockstudio" / "config.ini";
  }

  std::string getString(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? static_cast<std::string>(kf->get_string(group, key))
                                                             : std::string{};
  }

  std::int32_t getInt(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key, std::int32_t def)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? kf->get_integer(group, key) : def;
  }

  bool getBool(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key, bool def)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? kf->get_boolean(group, key) : def;
  }

  std::vector<std::string> getStringList(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key)
  {
    if (!kf->has_group(group) || !kf->has_key(group, key))
    {
      return {};
    }

    auto result = std::vector<std::string>{};
    for (auto const& value : kf->get_string_list(group, key))
    {
      result.emplace_back(value);
    }

    return result;
  }

  std::vector<Glib::ustring> toUstrings(std::vector<std::string> const& values)
  {
    auto result = std::vector<Glib::ustring>{};
    result.reserve(values.size());

    for (auto const& value : values)
    {
      result.emplace_back(value);
    }

    return result;
  }

  std::string encodeColumnWidth(std::pair<std::string, std::int32_t> const& width)
  {
    return std::format("{}:{}", width.first, width.second);
  }

  std::optional<std::pair<std::string, std::int32_t>> decodeColumnWidth(std::string const& text)
  {
    auto const separator = text.find(':');

    if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size())
    {
      return std::nullopt;
    }

    try
    {
      return std::pair<std::string, std::int32_t>{text.substr(0, separator), std::stoi(text.substr(separator + 1))};
    }
    catch (...)
    {
      return std::nullopt;
    }
  }
}

namespace app::core
{
  AppConfig AppConfig::load()
  {
    auto config = AppConfig{};
    auto const path = configPath();

    if (!std::filesystem::exists(path))
    {
      return config;
    }

    auto keyFile = Glib::KeyFile::create();
    keyFile->load_from_file(path.string());

    auto& ss = config._sessionState;
    auto& ws = config._windowState;
    auto& tvs = config._trackViewState;

    ss.lastLibraryPath = getString(keyFile, kAppGroup, kLastLibraryKey);
    ss.lastBackend = getString(keyFile, kAppGroup, kLastBackendKey);
    ss.lastOutputDeviceId = getString(keyFile, kAppGroup, kLastDeviceIdKey);
    ws.width = getInt(keyFile, kWindowGroup, kWidthKey, kDefaultWindowWidth);
    ws.height = getInt(keyFile, kWindowGroup, kHeightKey, kDefaultWindowHeight);
    ws.maximized = getBool(keyFile, kWindowGroup, kMaximizedKey, false);
    ws.panedPosition = getInt(keyFile, kWindowGroup, kPanedPositionKey, kDefaultPanedPosition);
    tvs.columnOrder = getStringList(keyFile, kTrackViewGroup, kColumnOrderKey);
    tvs.hiddenColumns = getStringList(keyFile, kTrackViewGroup, kHiddenColumnsKey);

    for (auto const& entry : getStringList(keyFile, kTrackViewGroup, kColumnWidthsKey))
    {
      if (auto width = decodeColumnWidth(entry))
      {
        tvs.columnWidths.insert_or_assign(width->first, width->second);
      }
    }

    return config;
  }

  void AppConfig::save() const
  {
    auto const path = configPath();
    APP_LOG_INFO("Saving config to: {}", path.string());
    std::filesystem::create_directories(path.parent_path());

    auto keyFile = Glib::KeyFile::create();
    keyFile->set_integer(kAppGroup, kConfigVersionKey, kAppConfigVersion);
    keyFile->set_string(kAppGroup, kLastLibraryKey, _sessionState.lastLibraryPath);
    keyFile->set_string(kAppGroup, kLastBackendKey, _sessionState.lastBackend);
    keyFile->set_string(kAppGroup, kLastDeviceIdKey, _sessionState.lastOutputDeviceId);

    keyFile->set_integer(kWindowGroup, kWidthKey, _windowState.width);
    keyFile->set_integer(kWindowGroup, kHeightKey, _windowState.height);
    keyFile->set_boolean(kWindowGroup, kMaximizedKey, _windowState.maximized);
    keyFile->set_integer(kWindowGroup, kPanedPositionKey, _windowState.panedPosition);

    keyFile->set_string_list(kTrackViewGroup, kColumnOrderKey, toUstrings(_trackViewState.columnOrder));
    keyFile->set_string_list(kTrackViewGroup, kHiddenColumnsKey, toUstrings(_trackViewState.hiddenColumns));

    auto widthEntries = std::vector<std::string>{};
    widthEntries.reserve(_trackViewState.columnWidths.size());
    for (auto const& width : _trackViewState.columnWidths)
    {
      widthEntries.push_back(encodeColumnWidth(width));
    }
    keyFile->set_string_list(kTrackViewGroup, kColumnWidthsKey, toUstrings(widthEntries));

    keyFile->save_to_file(path.string());
  }
}
