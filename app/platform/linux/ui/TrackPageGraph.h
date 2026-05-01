// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/services/PlaylistExporter.h"
#include "platform/linux/ui/LibrarySession.h"
#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackPresentation.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <gtkmm.h>
#include <rs/model/TrackIdList.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace app::ui
{
  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    std::unique_ptr<rs::model::TrackIdList> membershipList;
    std::unique_ptr<TrackListAdapter> adapter;
    std::unique_ptr<TrackViewPage> page;
    std::unique_ptr<app::services::PlaylistExporter> exporter;
  };

  /**
   * TrackPageGraph manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageGraph final
  {
  public:
    struct Callbacks final
    {
      std::function<void(std::vector<rs::TrackId> const&)> onSelectionChanged;
      std::function<void(TrackViewPage&, double, double)> onContextMenuRequested;
      std::function<void(TrackViewPage&, std::vector<rs::TrackId> const&, double, double)> onTagEditRequested;
      std::function<void(TrackViewPage&, rs::TrackId)> onTrackActivated;
      std::function<void(TrackViewPage&, std::string const&)> onCreateSmartListRequested;
    };

    TrackPageGraph(Gtk::Stack& stack, TrackColumnLayoutModel& layoutModel, Callbacks callbacks);
    ~TrackPageGraph();

    void clear();
    void rebuild(LibrarySession& session, rs::lmdb::ReadTransaction& txn);

    TrackPageContext* find(rs::ListId listId);
    TrackPageContext const* find(rs::ListId listId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void show(rs::ListId listId);

  private:
    void buildPageForAllTracks(LibrarySession& session);
    void buildPageForStoredList(rs::ListId listId, rs::library::ListView const& view, LibrarySession& session);
    void bindTrackPage(TrackPageContext& ctx);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    Callbacks _callbacks;

    std::map<rs::ListId, TrackPageContext> _trackPages;
  };
} // namespace app::ui
