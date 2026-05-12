// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackListAdapter.h"
#include "TrackPresentation.h"
#include "TrackViewPage.h"
#include "service/PlaylistExporter.h"
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
  class PlaybackController;
  class TagEditController;
  class TrackRowDataProvider;

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
   * TrackPageGraph manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageGraph final
  {
  public:
    TrackPageGraph(Gtk::Stack& stack,
                   TrackColumnLayoutModel& layoutModel,
                   ao::rt::AppSession& session,
                   PlaybackController* playbackController,
                   TagEditController& tagEditController,
                   ListSidebarController& listSidebar);
    ~TrackPageGraph();

    void setPlaybackController(PlaybackController& c) { _playbackController = &c; }

    Gtk::Stack& stack() { return _stack; }

    void clear();
    void rebuild(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction const& txn);

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
    void ensureViewPage(ao::rt::ViewId viewId, TrackRowDataProvider& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(ao::rt::PlaybackService::RevealTrackRequested const& ev);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    ao::rt::AppSession& _session;
    PlaybackController* _playbackController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    ao::rt::Subscription _revealSub;
    ao::rt::Subscription _nowPlayingSub;
    ao::rt::Subscription _focusSub;
    ao::rt::Subscription _viewDestroyedSub;
    ao::rt::Subscription _projectionChangedSub;

    std::map<ao::rt::ViewId, TrackPageContext> _trackPages;
    std::optional<ao::TrackId> _optPlayingTrackId;
    TrackRowDataProvider* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
