// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackViewPage.h"
#include "library_io/PlaylistExporter.h"
#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>
#include <runtime/PlaybackService.h>

#include <ao/library/ListView.h>
#include <gtkmm.h>
#include <runtime/TrackSource.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk
{
  class ListSidebarController;
  class PlaybackSequenceController;
  class TagEditController;
  class TrackRowCache;

  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    ao::rt::ViewId viewId{};
    std::unique_ptr<TrackListAdapter> adapter = {};
    std::unique_ptr<TrackViewPage> page = {};
    std::unique_ptr<ao::gtk::service::PlaylistExporter> exporter = {};
  };

  /**
   * TrackPageManager manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageManager final
  {
  public:
    TrackPageManager(Gtk::Stack& stack,
                   TrackColumnLayoutModel& layoutModel,
                   ao::rt::AppSession& session,
                   PlaybackSequenceController* sequenceController,
                   TagEditController& tagEditController,
                   ListSidebarController& listSidebar);
    ~TrackPageManager();

    void setPlaybackSequenceController(PlaybackSequenceController& c) { _playbackSequenceController = &c; }

    Gtk::Stack& stack() { return _stack; }

    void clear();
    void rebuild(TrackRowCache& dataProvider, ao::lmdb::ReadTransaction const& txn);

    TrackPageContext* find(ao::rt::ViewId viewId);
    TrackPageContext const* find(ao::rt::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(std::optional<ao::TrackId> trackId);

    /**
     * @return The list ID of the currently visible page, or allTracksListId() if none.
     */
    ao::ListId activeListId() const;

  private:
    void ensureViewPage(ao::rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(ao::rt::PlaybackService::RevealTrackRequested const& ev);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    ao::rt::AppSession& _session;
    PlaybackSequenceController* _playbackSequenceController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    ao::rt::Subscription _revealSub;
    ao::rt::Subscription _nowPlayingSub;
    ao::rt::Subscription _focusSub;
    ao::rt::Subscription _viewDestroyedSub;
    ao::rt::Subscription _projectionChangedSub;

    std::map<ao::rt::ViewId, TrackPageContext> _trackPages;
    std::optional<ao::TrackId> _optPlayingTrackId;
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
