// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AppConfig.h"

#include "UIState.h"
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::gtk
{
  AppConfig::AppConfig(std::filesystem::path const& configPath)
    : _storePtr{std::make_unique<rt::ConfigStore>(configPath)}
  {
  }

  AppConfig::~AppConfig() = default;

  AppConfig::AppConfig(AppConfig&&) noexcept = default;
  AppConfig& AppConfig::operator=(AppConfig&&) noexcept = default;

  void AppConfig::loadWindow(WindowState& state) const
  {
    if (auto const res = _storePtr->load("window", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfig: Failed to load window config: {}", res.error().message);
    }
  }

  void AppConfig::saveWindow(WindowState const& state)
  {
    _storePtr->save("window", state);

    if (auto const res = _storePtr->flush(); !res)
    {
      APP_LOG_ERROR("AppConfig: Failed to flush window config: {}", res.error().message);
    }
  }

  void AppConfig::loadAppPrefs(rt::AppPrefsState& state) const
  {
    if (auto const res = _storePtr->load("runtime", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfig: Failed to load app prefs: {}", res.error().message);
    }
  }

  void AppConfig::saveAppPrefs(rt::AppPrefsState const& state)
  {
    _storePtr->save("runtime", state);

    if (auto const res = _storePtr->flush(); !res)
    {
      APP_LOG_ERROR("AppConfig: Failed to flush app prefs: {}", res.error().message);
    }
  }

  uimodel::KeymapModel AppConfig::loadKeymap(uimodel::KeymapBindings defaults) const
  {
    return uimodel::loadKeymap(*_storePtr, std::move(defaults));
  }

  void AppConfig::saveKeymap(uimodel::KeymapModel const& keymap)
  {
    uimodel::saveKeymap(*_storePtr, keymap);
  }
} // namespace ao::gtk
