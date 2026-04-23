// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/AppConfig.h"
#include "core/Log.h"

#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include <filesystem>

namespace
{
  constexpr auto kAppGroup = "app";
  constexpr auto kWindowGroup = "window";

  constexpr auto kConfigVersionKey = "config_version";
  constexpr auto kLastLibraryKey = "last_library";
  constexpr auto kWidthKey = "width";
  constexpr auto kHeightKey = "height";
  constexpr auto kMaximizedKey = "maximized";
  constexpr auto kPanedPositionKey = "paned_position";

  constexpr std::int32_t kAppConfigVersion = 1;

  std::filesystem::path configPath()
  {
    return std::filesystem::path{Glib::get_user_config_dir()} / "rockstudio" / "config.ini";
  }

  std::string getString(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? static_cast<std::string>(kf->get_string(group, key)) : std::string{};
  }

  int getInt(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key, int def)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? kf->get_integer(group, key) : def;
  }

  bool getBool(Glib::RefPtr<Glib::KeyFile> const& kf, char const* group, char const* key, bool def)
  {
    return (kf->has_group(group) && kf->has_key(group, key)) ? kf->get_boolean(group, key) : def;
  }
}

namespace app::core
{
  AppConfig AppConfig::load()
  {
    auto config = AppConfig{};
    auto const path = configPath();
    if (!std::filesystem::exists(path)) return config;

    auto keyFile = Glib::KeyFile::create();
    keyFile->load_from_file(path.string());

    auto& ss = config._sessionState;
    auto& ws = config._windowState;

    ss.lastLibraryPath = getString(keyFile, kAppGroup, kLastLibraryKey);
    ws.width = getInt(keyFile, kWindowGroup, kWidthKey, kDefaultWindowWidth);
    ws.height = getInt(keyFile, kWindowGroup, kHeightKey, kDefaultWindowHeight);
    ws.maximized = getBool(keyFile, kWindowGroup, kMaximizedKey, false);
    ws.panedPosition = getInt(keyFile, kWindowGroup, kPanedPositionKey, kDefaultPanedPosition);

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

    keyFile->set_integer(kWindowGroup, kWidthKey, _windowState.width);
    keyFile->set_integer(kWindowGroup, kHeightKey, _windowState.height);
    keyFile->set_boolean(kWindowGroup, kMaximizedKey, _windowState.maximized);
    keyFile->set_integer(kWindowGroup, kPanedPositionKey, _windowState.panedPosition);

    keyFile->save_to_file(path.string());
  }
}
