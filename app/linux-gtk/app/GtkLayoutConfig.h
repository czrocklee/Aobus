// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "UIState.h"

#include <filesystem>
#include <memory>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk
{
  /**
   * @brief Manages persistence of GTK layout state (column widths, etc.).
   * 
   * This class exclusively owns the `gtk_layout.yaml` file within a library.
   */
  class GtkLayoutConfig final
  {
  public:
    explicit GtkLayoutConfig(std::filesystem::path const& libraryPath);
    ~GtkLayoutConfig();

    GtkLayoutConfig(GtkLayoutConfig const&) = delete;
    GtkLayoutConfig& operator=(GtkLayoutConfig const&) = delete;
    GtkLayoutConfig(GtkLayoutConfig&&) noexcept;
    GtkLayoutConfig& operator=(GtkLayoutConfig&&) noexcept;

    /**
     * @brief Load the layout state from disk.
     */
    void load(ColumnLayoutState& state) const;

    /**
     * @brief Save the layout state to disk and flush.
     */
    void save(ColumnLayoutState const& state);

  private:
    std::unique_ptr<rt::ConfigStore> _store;
  };
} // namespace ao::gtk
