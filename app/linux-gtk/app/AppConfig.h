// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "UIState.h"
#include "layout/document/LayoutDocument.h"
#include "runtime/StateTypes.h"

#include <filesystem>
#include <memory>

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

    void loadShellLayout(layout::LayoutDocument& state) const;
    void saveShellLayout(layout::LayoutDocument const& state);

    /**
     * @brief Access the underlying store (for cases where direct access is needed, e.g. AppRuntime).
     * @todo Consider if we can avoid this by moving AppRuntime's persistence needs here.
     */
    rt::ConfigStore& store() const;

  private:
    std::unique_ptr<rt::ConfigStore> _store;
  };
} // namespace ao::gtk
