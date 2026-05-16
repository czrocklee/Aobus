// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <gtkmm/stack.h>
#include <runtime/AppRuntime.h>
#include <runtime/CorePrimitives.h>
#include <runtime/PlaybackService.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

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
  class PlaylistExporter;
  class TrackPresentationStore;
  class TrackColumnLayoutModel;

  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    rt::ViewId viewId{};
    std::unique_ptr<TrackListAdapter> adapter = {};
    std::unique_ptr<TrackViewPage> page = {};
    std::unique_ptr<PlaylistExporter> exporter = {};
  };

  /**
   * TrackPageHost manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageHost final
  {
  public:
    TrackPageHost(Gtk::Stack& stack,
                  TrackColumnLayoutModel& layoutModel,
                  rt::AppRuntime& runtime,
                  PlaybackSequenceController* sequenceController,
                  TagEditController& tagEditController,
                  ListSidebarController& listSidebar,
                  TrackPresentationStore& presentationStore);
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

    void clear();
    void rebuild(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn);

    TrackPageContext* find(rt::ViewId viewId);
    TrackPageContext const* find(rt::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(std::optional<TrackId> optTrackId);

    /**
     * @return The list ID of the currently visible page, or allTracksListId() if none.
     */
    ListId activeListId() const;

  private:
    void ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(rt::PlaybackService::RevealTrackRequested const& ev);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    rt::AppRuntime& _runtime;
    PlaybackSequenceController* _playbackSequenceController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    TrackPresentationStore& _presentationStore;
    rt::Subscription _revealSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _focusSub;
    rt::Subscription _viewDestroyedSub;
    rt::Subscription _projectionChangedSub;

    std::map<rt::ViewId, TrackPageContext> _trackPages;
    std::optional<TrackId> _optPlayingTrackId;
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
