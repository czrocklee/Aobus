// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#include "platform/linux/ui/TrackPageGraph.h"
#include <rs/utility/Log.h>
#include <rs/model/AllTrackIdsList.h>
#include <rs/model/FilteredTrackIdList.h>
#include <rs/model/ManualTrackIdList.h>

#include <format>
#include <limits>

namespace app::ui
{
  namespace
  {
    rs::ListId allTracksListId()
    {
      return rs::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    std::string pageNameForListId(rs::ListId listId)
    {
      if (listId == allTracksListId())
      {
        return "all-tracks";
      }

      return "list-" + std::format("{}", listId.value());
    }
  }

  TrackPageGraph::TrackPageGraph(Gtk::Stack& stack, TrackColumnLayoutModel& layoutModel, Callbacks callbacks)
    : _stack(stack)
    , _layoutModel(layoutModel)
    , _callbacks(std::move(callbacks))
  {
  }

  TrackPageGraph::~TrackPageGraph()
  {
    clear();
  }

  void TrackPageGraph::clear()
  {
    while (!_trackPages.empty())
    {
      auto it = std::prev(_trackPages.end());

      if (it->second.page)
      {
        _stack.remove(*it->second.page);
      }

      _trackPages.erase(it);
    }
  }

  void TrackPageGraph::rebuild(LibrarySession& session, rs::lmdb::ReadTransaction& txn)
  {
    APP_LOG_DEBUG("TrackPageGraph::rebuild called");
    clear();

    buildPageForAllTracks(session);

    auto reader = session.musicLibrary->lists().reader(txn);
    for (auto const& [id, listView] : reader)
    {
      buildPageForStoredList(id, listView, session);
    }
  }

  TrackPageContext* TrackPageGraph::find(rs::ListId listId)
  {
    auto it = _trackPages.find(listId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext const* TrackPageGraph::find(rs::ListId listId) const
  {
    auto it = _trackPages.find(listId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext* TrackPageGraph::currentVisible()
  {
    auto* visibleChild = _stack.get_visible_child();
    if (!visibleChild) return nullptr;

    for (auto& [id, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild) return &ctx;
    }
    return nullptr;
  }

  TrackPageContext const* TrackPageGraph::currentVisible() const
  {
    auto const* visibleChild = _stack.get_visible_child();
    if (!visibleChild) return nullptr;

    for (auto const& [id, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild) return &ctx;
    }
    return nullptr;
  }

  void TrackPageGraph::show(rs::ListId listId)
  {
    _stack.set_visible_child(pageNameForListId(listId));
  }

  void TrackPageGraph::buildPageForAllTracks(LibrarySession& session)
  {
    auto adapter = std::make_unique<TrackListAdapter>(*session.allTrackIds, *session.rowDataProvider);
    adapter->onReset();
    auto trackPage = std::make_unique<TrackViewPage>(allTracksListId(), *adapter, _layoutModel);

    auto pageId = pageNameForListId(allTracksListId());
    _stack.add(*trackPage, pageId, "All Tracks");

    TrackPageContext ctx;
    ctx.membershipList = nullptr;
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);
    
    bindTrackPage(ctx);
    _trackPages[allTracksListId()] = std::move(ctx);
  }

  void TrackPageGraph::buildPageForStoredList(rs::ListId listId, rs::library::ListView const& view, LibrarySession& session)
  {
    std::string listName = view.name().empty() ? "<Unnamed List>" : std::string(view.name());

    std::unique_ptr<rs::model::TrackIdList> membershipList;
    if (view.isSmart())
    {
      auto* sourceList = static_cast<rs::model::TrackIdList*>(session.allTrackIds.get());
      if (!view.isRootParent())
      {
        if (auto* sourceCtx = find(view.parentId()))
        {
          if (sourceCtx->membershipList) sourceList = sourceCtx->membershipList.get();
        }
      }

      auto filtered = std::make_unique<rs::model::FilteredTrackIdList>(*sourceList, *session.musicLibrary, *session.smartListEngine);
      filtered->setExpression(std::string(view.filter()));
      filtered->reload();
      membershipList = std::move(filtered);
    }
    else
    {
      auto manual = std::make_unique<rs::model::ManualTrackIdList>(view, session.allTrackIds.get());
      membershipList = std::move(manual);
    }

    auto adapter = std::make_unique<TrackListAdapter>(*membershipList, *session.rowDataProvider);
    adapter->onReset();

    auto trackPage = std::make_unique<TrackViewPage>(listId, *adapter, _layoutModel);
    auto pageId = pageNameForListId(listId);
    _stack.add(*trackPage, pageId, listName);

    auto playlistDir = session.musicLibrary->rootPath() / "playlist";
    if (!std::filesystem::exists(playlistDir)) std::filesystem::create_directories(playlistDir);
    auto playlistPath = playlistDir / (listName + ".m3u");
    auto exporter = std::make_unique<app::services::PlaylistExporter>(
      *membershipList, *session.rowDataProvider, session.musicLibrary->rootPath(), playlistPath);

    TrackPageContext ctx;
    ctx.membershipList = std::move(membershipList);
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);
    ctx.exporter = std::move(exporter);

    bindTrackPage(ctx);
    _trackPages[listId] = std::move(ctx);
  }

  void TrackPageGraph::bindTrackPage(TrackPageContext& ctx)
  {
    auto* page = ctx.page.get();
    
    page->signalSelectionChanged().connect([this, page] {
      if (_callbacks.onSelectionChanged) _callbacks.onSelectionChanged(page->getSelectedTrackIds());
    });

    page->signalContextMenuRequested().connect([this, page](double x, double y) {
      if (_callbacks.onContextMenuRequested) _callbacks.onContextMenuRequested(*page, x, y);
    });

    page->signalTagEditRequested().connect([this, page](std::vector<rs::TrackId> const& ids, double x, double y) {
      if (_callbacks.onTagEditRequested) _callbacks.onTagEditRequested(*page, ids, x, y);
    });

    page->signalTrackActivated().connect([this, page](rs::TrackId id) {
      if (_callbacks.onTrackActivated) _callbacks.onTrackActivated(*page, id);
    });
  }
} // namespace app::ui
