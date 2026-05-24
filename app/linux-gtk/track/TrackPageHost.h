// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "portal/PlaylistExporter.h"
#include "runtime/AppRuntime.h"
#include "runtime/CorePrimitives.h"
#include "runtime/PlaybackService.h"
#include "track/TrackViewPage.h"

#include <gtkmm/stack.h>

#include <map>
#include <memory>

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::gtk
{
  class ListSidebarController;
  class PlaybackSequenceController;
  class TagEditController;
  class TrackRowCache;
  class TrackListAdapter;
  class TrackViewPage;
  class ImageCache;

  namespace portal
  {
    class PlaylistExporter;
  }
  class TrackPresentationStore;

  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    rt::ViewId viewId{};
    std::unique_ptr<TrackListAdapter> adapter = {};
    std::unique_ptr<TrackViewPage> page = {};
    std::unique_ptr<portal::PlaylistExporter> exporter = {};
  };

  /**
   * TrackPageHost manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageHost final
  {
  public:
    TrackPageHost(Gtk::Stack& stack,
                  rt::AppRuntime& runtime,
                  PlaybackSequenceController* sequenceController,
                  TagEditController& tagEditController,
                  ListSidebarController& listSidebar,
                  TrackPresentationStore& presentationStore,
                  ImageCache* imageCache);
    ~TrackPageHost();

    // Not copyable or movable
    TrackPageHost(TrackPageHost const&) = delete;
    TrackPageHost& operator=(TrackPageHost const&) = delete;
    TrackPageHost(TrackPageHost&&) = delete;
    TrackPageHost& operator=(TrackPageHost&&) = delete;

    void setPlaybackSequenceController(PlaybackSequenceController& controller)
    {
      _playbackSequenceController = &controller;
    }

    Gtk::Stack& stack() { return _stack; }
    TrackPresentationStore& presentationStore() { return _presentationStore; }

    void clear();
    void rebuild(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn);

    TrackPageContext* find(rt::ViewId viewId);
    TrackPageContext const* find(rt::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(TrackId trackId);

    /**
     * @return The list ID of the currently visible page, or rt::kAllTracksListId if none.
     */
    ListId activeListId() const;

  private:
    void ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(rt::PlaybackService::RevealTrackRequested const& ev);

    Gtk::Stack& _stack;
    rt::AppRuntime& _runtime;
    PlaybackSequenceController* _playbackSequenceController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    TrackPresentationStore& _presentationStore;
    ImageCache* _imageCache = nullptr;
    rt::Subscription _revealSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _focusSub;
    rt::Subscription _viewDestroyedSub;
    rt::Subscription _projectionChangedSub;
    rt::Subscription _presentationChangedSub;

    std::map<rt::ViewId, TrackPageContext> _trackPages;
    TrackId _playingTrackId{kInvalidTrackId};
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
