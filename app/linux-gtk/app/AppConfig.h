// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "UIState.h"
#include "layout/document/LayoutDocument.h"
#include <ao/rt/StateTypes.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk
{
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

    bool loadShellLayout(layout::LayoutDocument& state, std::string_view presetId) const;
    void saveShellLayout(layout::LayoutDocument const& state, std::string_view presetId);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };
} // namespace ao::gtk
