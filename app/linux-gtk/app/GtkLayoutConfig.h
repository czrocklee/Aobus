// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

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
    void load(uimodel::TrackColumnLayoutState& layoutState, uimodel::ListPresentationPreferenceState& prefState) const;

    /**
     * @brief Save the layout state to disk and flush.
     */
    void save(uimodel::TrackColumnLayoutState const& layoutState,
              uimodel::ListPresentationPreferenceState const& prefState);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };
} // namespace ao::gtk
