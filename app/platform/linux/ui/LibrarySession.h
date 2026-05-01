// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/ui/TrackRowDataProvider.h"
#include <rs/library/MusicLibrary.h>
#include <rs/model/AllTrackIdsList.h>
#include <rs/model/SmartListEngine.h>

#include <filesystem>
#include <memory>

namespace app::ui
{
  /**
   * LibrarySession encapsulates the active library runtime state.
   *
   * By grouping these members, opening or replacing a library becomes
   * a single atomic operation from the perspective of the MainWindow.
   */
  struct LibrarySession final
  {
    std::unique_ptr<rs::library::MusicLibrary> musicLibrary;
    std::unique_ptr<TrackRowDataProvider> rowDataProvider;
    std::unique_ptr<rs::model::AllTrackIdsList> allTrackIds;
    std::unique_ptr<rs::model::SmartListEngine> smartListEngine;
  };

  /**
   * Factory function that builds a ready-to-use session from a library path.
   */
  std::unique_ptr<LibrarySession> makeLibrarySession(std::filesystem::path const& rootPath);
} // namespace app::ui
