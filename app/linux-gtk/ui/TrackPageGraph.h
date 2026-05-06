// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "PlaylistExporter.h"
#include "TrackListAdapter.h"
#include "TrackPresentation.h"
#include "TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/CommandTypes.h>
#include <runtime/CorePrimitives.h>

#include <ao/library/ListView.h>
#include <ao/model/TrackIdList.h>
#include <gtkmm.h>

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
    std::shared_ptr<ao::model::TrackIdList> membershipList;
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
    struct Callbacks final
    {
      std::function<void(std::vector<ao::TrackId> const&)> onSelectionChanged;
      std::function<void(TrackViewPage&, double, double)> onContextMenuRequested;
      std::function<void(TrackViewPage&, std::vector<ao::TrackId> const&, Gtk::Widget*)> onTagEditRequested;
      std::function<void(TrackViewPage&, ao::TrackId)> onTrackActivated;
      std::function<void(TrackViewPage&, std::string const&)> onCreateSmartListRequested;
    };

    TrackPageGraph(Gtk::Stack& stack,
                   TrackColumnLayoutModel& layoutModel,
                   ao::app::AppSession& session,
                   PlaybackController* playbackController,
                   TagEditController& tagEditController,
                   ListSidebarController& listSidebar,
                   Callbacks callbacks);
    ~TrackPageGraph();

    void setPlaybackController(PlaybackController& c) { _playbackController = &c; }

    void clear();
    void rebuild(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction& txn);

    TrackPageContext* find(ao::ListId listId);
    TrackPageContext const* find(ao::ListId listId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void show(ao::ListId listId);
    void setPlayingTrack(std::optional<ao::TrackId> trackId);

  private:
    void buildPageForAllTracks(TrackRowDataProvider& dataProvider);
    void buildPageForStoredList(ao::ListId listId,
                                ao::library::ListView const& view,
                                TrackRowDataProvider& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    ao::app::AppSession& _session;
    PlaybackController* _playbackController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    ao::app::Subscription _revealSub;
    ao::app::Subscription _nowPlayingSub;
    Callbacks _callbacks;

    std::map<ao::ListId, TrackPageContext> _trackPages;
    std::optional<ao::TrackId> _playingTrackId;
  };
} // namespace ao::gtk
