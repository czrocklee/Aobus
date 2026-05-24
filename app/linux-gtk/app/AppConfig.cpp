// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AppConfig.h"

#include "UIState.h"
#include "ao/utility/Log.h"
#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutYaml.h" // NOLINT(misc-include-cleaner)
#include "runtime/ConfigStore.h"
#include "runtime/StateTypes.h"

#include <filesystem>
#include <memory>

namespace ao::gtk
{
  AppConfig::AppConfig(std::filesystem::path const& configPath)
    : _store{std::make_unique<rt::ConfigStore>(configPath)}
  {
  }

  AppConfig::~AppConfig() = default;

  AppConfig::AppConfig(AppConfig&&) noexcept = default;
  AppConfig& AppConfig::operator=(AppConfig&&) noexcept = default;

  void AppConfig::loadWindow(WindowState& state) const
  {
    if (auto const res = _store->load("window", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfig: Failed to load window config: {}", res.error().message);
    }
  }

  void AppConfig::saveWindow(WindowState const& state)
  {
    _store->save("window", state);

    if (auto const res = _store->flush(); !res)
    {
      APP_LOG_ERROR("AppConfig: Failed to flush window config: {}", res.error().message);
    }
  }

  void AppConfig::loadAppPrefs(rt::AppPrefsState& state) const
  {
    if (auto const res = _store->load("runtime", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfig: Failed to load app prefs: {}", res.error().message);
    }
  }

  void AppConfig::saveAppPrefs(rt::AppPrefsState const& state)
  {
    _store->save("runtime", state);

    if (auto const res = _store->flush(); !res)
    {
      APP_LOG_ERROR("AppConfig: Failed to flush app prefs: {}", res.error().message);
    }
  }

  void AppConfig::loadShellLayout(layout::LayoutDocument& state) const
  {
    if (auto const res = _store->load("linuxGtkLayout", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("AppConfig: Failed to load shell layout: {}", res.error().message);
    }
  }

  void AppConfig::saveShellLayout(layout::LayoutDocument const& state)
  {
    _store->save("linuxGtkLayout", state);

    if (auto const res = _store->flush(); !res)
    {
      APP_LOG_ERROR("AppConfig: Failed to flush shell layout: {}", res.error().message);
    }
  }

  rt::ConfigStore& AppConfig::store() const
  {
    return *_store;
  }
} // namespace ao::gtk
