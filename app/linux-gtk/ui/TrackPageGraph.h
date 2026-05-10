// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "PlaylistExporter.h"
#include "TrackListAdapter.h"
#include "TrackPresentation.h"
#include "TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>

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
    ao::app::ViewId viewId{};
    std::unique_ptr<TrackListAdapter> adapter;
    std::unique_ptr<TrackViewPage> page;
    std::unique_ptr<ao::gtk::services::PlaylistExporter> exporter;
  };

  /**
   * TrackPageGraph manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageGraph final
  {
  public:
    TrackPageGraph(Gtk::Stack& stack,
                   TrackColumnLayoutModel& layoutModel,
                   ao::app::AppSession& session,
                   PlaybackController* playbackController,
                   TagEditController& tagEditController,
                   ListSidebarController& listSidebar);
    ~TrackPageGraph();

    void setPlaybackController(PlaybackController& c) { _playbackController = &c; }

    void clear();
    void rebuild(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction const& txn);

    TrackPageContext* find(ao::app::ViewId viewId);
    TrackPageContext const* find(ao::app::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(std::optional<ao::TrackId> trackId);

  private:
    void ensureViewPage(ao::app::ViewId viewId, TrackRowDataProvider& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    ao::app::AppSession& _session;
    PlaybackController* _playbackController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    ao::app::Subscription _revealSub;
    ao::app::Subscription _nowPlayingSub;
    ao::app::Subscription _focusSub;
    ao::app::Subscription _viewDestroyedSub;

    std::map<ao::app::ViewId, TrackPageContext> _trackPages;
    std::optional<ao::TrackId> _optPlayingTrackId;
    TrackRowDataProvider* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
