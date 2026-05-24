// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk
{
  class TrackRowCache;
  class ImageCache;
  class PlaybackSequenceController;
  class TagEditController;
  namespace portal
  {
    class ImportExportCoordinator;
  }
  class TrackPageHost;
  class TrackPresentationStore;
  class ListSidebarController;

  /**
   * A bag of services provided by the GTK application layer to be consumed by
   * the UI layout components.
   */
  struct GtkUiServices final
  {
    TrackRowCache* trackRowCache = nullptr;
    ImageCache* imageCache = nullptr;
    PlaybackSequenceController* playbackSequenceController = nullptr;
    TagEditController* tagEditController = nullptr;
    portal::ImportExportCoordinator* importExportCoordinator = nullptr;
    TrackPageHost* trackPageHost = nullptr;
    TrackPresentationStore* trackPresentationStore = nullptr;
    ListSidebarController* listSidebarController = nullptr;
  };
} // namespace ao::gtk
