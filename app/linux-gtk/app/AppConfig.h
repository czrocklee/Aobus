// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeymapModel.h>

#include <filesystem>
#include <memory>

namespace ao::rt
{
  struct AppPrefsState;
  struct AppSessionState;
}

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk
{
  struct WindowState;

  /**
   * @brief Manages persistence of global application configuration.
   *
   * This class exclusively owns the global `config.yaml` file.
   */
  class AppConfig final
  {
  public:
    explicit AppConfig(std::filesystem::path const& configPath);
    ~AppConfig();

    AppConfig(AppConfig const&) = delete;
    AppConfig& operator=(AppConfig const&) = delete;
    AppConfig(AppConfig&&) noexcept;
    AppConfig& operator=(AppConfig&&) noexcept;

    void loadWindow(WindowState& state) const;
    void saveWindow(WindowState const& state);

    void loadAppPrefs(rt::AppPrefsState& state) const;
    void saveAppPrefs(rt::AppPrefsState const& state);
    void loadAppSession(rt::AppSessionState& state) const;
    void saveAppSession(rt::AppSessionState const& state);

    /// Loads the effective keyboard map: persisted overrides merged onto @p defaults.
    uimodel::KeymapModel loadKeymap(uimodel::KeymapBindings defaults) const;
    /// Persists the keymap's delta-from-defaults into the `shortcuts` group.
    void saveKeymap(uimodel::KeymapModel const& keymap);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };
} // namespace ao::gtk
