// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/services/PlaylistExporter.h"
#include "platform/linux/ui/LibrarySession.h"
#include "platform/linux/ui/TrackListAdapter.h"
#include "platform/linux/ui/TrackPresentation.h"
#include "platform/linux/ui/TrackViewPage.h"

#include <ao/model/TrackIdList.h>
#include <gtkmm.h>

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
    std::unique_ptr<ao::model::TrackIdList> membershipList;
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
      std::function<void(std::vector<ao::TrackId> const&)> onSelectionChanged;
      std::function<void(TrackViewPage&, double, double)> onContextMenuRequested;
      std::function<void(TrackViewPage&, std::vector<ao::TrackId> const&, double, double)> onTagEditRequested;
      std::function<void(TrackViewPage&, ao::TrackId)> onTrackActivated;
      std::function<void(TrackViewPage&, std::string const&)> onCreateSmartListRequested;
    };

    TrackPageGraph(Gtk::Stack& stack, TrackColumnLayoutModel& layoutModel, Callbacks callbacks);
    ~TrackPageGraph();

    void clear();
    void rebuild(LibrarySession& session, ao::lmdb::ReadTransaction& txn);

    TrackPageContext* find(ao::ListId listId);
    TrackPageContext const* find(ao::ListId listId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void show(ao::ListId listId);

  private:
    void buildPageForAllTracks(LibrarySession& session);
    void buildPageForStoredList(ao::ListId listId, ao::library::ListView const& view, LibrarySession& session);
    void bindTrackPage(TrackPageContext& ctx);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    Callbacks _callbacks;

    std::map<ao::ListId, TrackPageContext> _trackPages;
  };
} // namespace app::ui
