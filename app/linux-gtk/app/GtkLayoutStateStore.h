// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/utility/Uuid.h>

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
   * This class exclusively owns the `gtk_layout.yaml` file within a library,
   * and stamps what it saves with the id of the library that file belongs to.
   */
  class GtkLayoutStateStore final
  {
  public:
    GtkLayoutStateStore(std::filesystem::path const& libraryPath, utility::UuidBytes const& libraryId);
    ~GtkLayoutStateStore();

    GtkLayoutStateStore(GtkLayoutStateStore const&) = delete;
    GtkLayoutStateStore& operator=(GtkLayoutStateStore const&) = delete;
    GtkLayoutStateStore(GtkLayoutStateStore&&) noexcept;
    GtkLayoutStateStore& operator=(GtkLayoutStateStore&&) noexcept;

    /**
     * @brief Load the layout state from disk.
     */
    void load(uimodel::TrackColumnLayouts::Snapshot& layoutState,
              uimodel::ListPresentations::Snapshot& prefState) const;

    /**
     * @brief Save the layout state to disk.
     */
    void save(uimodel::TrackColumnLayouts::Snapshot const& layoutState,
              uimodel::ListPresentations::Snapshot const& prefState);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
    utility::UuidBytes _libraryId;
  };
} // namespace ao::gtk
