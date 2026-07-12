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
  class GtkLayoutStateStore final
  {
  public:
    explicit GtkLayoutStateStore(std::filesystem::path const& libraryPath);
    ~GtkLayoutStateStore();

    GtkLayoutStateStore(GtkLayoutStateStore const&) = delete;
    GtkLayoutStateStore& operator=(GtkLayoutStateStore const&) = delete;
    GtkLayoutStateStore(GtkLayoutStateStore&&) noexcept;
    GtkLayoutStateStore& operator=(GtkLayoutStateStore&&) noexcept;

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
