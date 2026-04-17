// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "AppConfig.h"

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

  constexpr int kAppConfigVersion = 1;

  auto configPath() -> std::filesystem::path
  {
    return std::filesystem::path{Glib::get_user_config_dir()} / "rockstudio" / "config.ini";
  }
}

namespace app
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

    if (keyFile->has_group(kAppGroup) && keyFile->has_key(kAppGroup, kLastLibraryKey))
    {
      config._sessionState.lastLibraryPath = keyFile->get_string(kAppGroup, kLastLibraryKey);
    }

    if (keyFile->has_group(kWindowGroup) && keyFile->has_key(kWindowGroup, kWidthKey))
    {
      config._windowState.width = keyFile->get_integer(kWindowGroup, kWidthKey);
    }

    if (keyFile->has_group(kWindowGroup) && keyFile->has_key(kWindowGroup, kHeightKey))
    {
      config._windowState.height = keyFile->get_integer(kWindowGroup, kHeightKey);
    }

    if (keyFile->has_group(kWindowGroup) && keyFile->has_key(kWindowGroup, kMaximizedKey))
    {
      config._windowState.maximized = keyFile->get_boolean(kWindowGroup, kMaximizedKey);
    }

    if (keyFile->has_group(kWindowGroup) && keyFile->has_key(kWindowGroup, kPanedPositionKey))
    {
      config._windowState.panedPosition = keyFile->get_integer(kWindowGroup, kPanedPositionKey);
    }

    return config;
  }

  void AppConfig::save() const
  {
    auto const path = configPath();
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
