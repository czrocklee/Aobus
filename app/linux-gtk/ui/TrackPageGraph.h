// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibrarySession.h"
#include "MetadataCoordinator.h"
#include "PlaylistExporter.h"
#include "TrackListAdapter.h"
#include "TrackPresentation.h"
#include "TrackViewPage.h"

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
  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    std::unique_ptr<ao::model::TrackIdList> membershipList;
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
                   MetadataCoordinator& metadataCoordinator,
                   Callbacks callbacks);
    ~TrackPageGraph();

    void clear();
    void rebuild(LibrarySession& session, ao::lmdb::ReadTransaction& txn);

    TrackPageContext* find(ao::ListId listId);
    TrackPageContext const* find(ao::ListId listId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void show(ao::ListId listId);
    void setPlayingTrack(std::optional<ao::TrackId> trackId);

  private:
    void buildPageForAllTracks(LibrarySession& session);
    void buildPageForStoredList(ao::ListId listId, ao::library::ListView const& view, LibrarySession& session);
    void bindTrackPage(TrackPageContext& ctx);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    MetadataCoordinator& _metadataCoordinator;
    Callbacks _callbacks;

    std::map<ao::ListId, TrackPageContext> _trackPages;
    std::optional<ao::TrackId> _playingTrackId;
  };
} // namespace ao::gtk
