// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk
{
  class TrackRowCache;
  class CoverArtCache;
  class PlaybackSequenceController;
  class TagEditController;
  class ImportExportCoordinator;
  class TrackPageHost;
  class TrackColumnLayoutModel;
  class ListSidebarController;

  /**
   * A bag of services provided by the GTK application layer to be consumed by
   * the UI layout components.
   */
  struct GtkUiServices final
  {
    TrackRowCache* trackRowCache = nullptr;
    CoverArtCache* coverArtCache = nullptr;
    PlaybackSequenceController* playbackSequenceController = nullptr;
    TagEditController* tagEditController = nullptr;
    ImportExportCoordinator* importExportCoordinator = nullptr;
    TrackPageHost* trackPageHost = nullptr;
    TrackColumnLayoutModel* columnLayoutModel = nullptr;
    ListSidebarController* listSidebarController = nullptr;
  };
} // namespace ao::gtk
