// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AppConfigStore.h"

#include "WindowState.h"
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::gtk
{
  AppConfigStore::AppConfigStore(std::filesystem::path const& configPath)
    : _storePtr{std::make_unique<rt::ConfigStore>(configPath)}
  {
  }

  AppConfigStore::~AppConfigStore() = default;

  AppConfigStore::AppConfigStore(AppConfigStore&&) noexcept = default;
  AppConfigStore& AppConfigStore::operator=(AppConfigStore&&) noexcept = default;

  void AppConfigStore::loadWindow(WindowState& state) const
  {
    if (auto const res = _storePtr->load("window", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfigStore: Failed to load window config: {}", res.error().message);
    }
  }

  void AppConfigStore::saveWindow(WindowState const& state)
  {
    if (auto const res = _storePtr->save("window", state); !res)
    {
      APP_LOG_ERROR("AppConfigStore: Failed to save window config: {}", res.error().message);
    }
  }

  void AppConfigStore::loadAppPrefs(rt::AppPrefsState& state) const
  {
    if (auto const res = _storePtr->load("runtime", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfigStore: Failed to load app prefs: {}", res.error().message);
    }
  }

  void AppConfigStore::saveAppPrefs(rt::AppPrefsState const& state)
  {
    if (auto const res = _storePtr->save("runtime", state); !res)
    {
      APP_LOG_ERROR("AppConfigStore: Failed to save app prefs: {}", res.error().message);
    }
  }

  void AppConfigStore::loadAppSession(rt::AppSessionState& state) const
  {
    if (auto const res = _storePtr->load("session", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfigStore: Failed to load app session: {}", res.error().message);
    }
  }

  void AppConfigStore::saveAppSession(rt::AppSessionState const& state)
  {
    if (auto const res = _storePtr->save("session", state); !res)
    {
      APP_LOG_ERROR("AppConfigStore: Failed to save app session: {}", res.error().message);
    }
  }

  rt::ConfigStore& AppConfigStore::playbackSessionStore() noexcept
  {
    return *_storePtr;
  }

  uimodel::KeymapModel AppConfigStore::loadKeymap(uimodel::KeymapBindings defaults) const
  {
    return uimodel::loadKeymap(*_storePtr, std::move(defaults));
  }

  void AppConfigStore::saveKeymap(uimodel::KeymapModel const& keymap)
  {
    uimodel::saveKeymap(*_storePtr, keymap);
  }
} // namespace ao::gtk
