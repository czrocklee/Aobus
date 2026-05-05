// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPageGraph.h"
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/model/AllTrackIdsList.h>
#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/ManualTrackIdList.h>
#include <ao/utility/Log.h>

#include <format>
#include <limits>

namespace ao::gtk
{
  namespace
  {
    ao::ListId allTracksListId()
    {
      return ao::ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    std::string pageNameForListId(ao::ListId listId)
    {
      if (listId == allTracksListId())
      {
        return "all-tracks";
      }

      return "list-" + std::format("{}", listId.value());
    }
  }

  TrackPageGraph::TrackPageGraph(Gtk::Stack& stack,
                                 TrackColumnLayoutModel& layoutModel,
                                 MetadataCoordinator& metadataCoordinator,
                                 Callbacks callbacks)
    : _stack{stack}
    , _layoutModel{layoutModel}
    , _metadataCoordinator{metadataCoordinator}
    , _callbacks{std::move(callbacks)}
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

  void TrackPageGraph::rebuild(LibrarySession& session, ao::lmdb::ReadTransaction& txn)
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

  TrackPageContext* TrackPageGraph::find(ao::ListId listId)
  {
    auto it = _trackPages.find(listId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext const* TrackPageGraph::find(ao::ListId listId) const
  {
    auto it = _trackPages.find(listId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext* TrackPageGraph::currentVisible()
  {
    auto* const visibleChild = _stack.get_visible_child();
    if (visibleChild == nullptr)
    {
      return nullptr;
    }

    for (auto& [id, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild)
      {
        return &ctx;
      }
    }

    return nullptr;
  }

  TrackPageContext const* TrackPageGraph::currentVisible() const
  {
    auto const* const visibleChild = _stack.get_visible_child();
    if (visibleChild == nullptr)
    {
      return nullptr;
    }

    for (auto const& [id, ctx] : _trackPages)
    {
      if (ctx.page.get() == visibleChild)
      {
        return &ctx;
      }
    }

    return nullptr;
  }

  void TrackPageGraph::show(ao::ListId listId)
  {
    _stack.set_visible_child(pageNameForListId(listId));
  }

  void TrackPageGraph::setPlayingTrack(std::optional<ao::TrackId> trackId)
  {
    if (_playingTrackId == trackId)
    {
      return;
    }

    _playingTrackId = trackId;

    for (auto& [id, ctx] : _trackPages)
    {
      if (ctx.page)
      {
        ctx.page->setPlayingTrackId(trackId);
      }
    }
  }

  void TrackPageGraph::buildPageForAllTracks(LibrarySession& session)
  {
    auto adapter =
      std::make_unique<TrackListAdapter>(*session.allTrackIds, *session.musicLibrary, *session.rowDataProvider);
    adapter->onReset();
    auto trackPage = std::make_unique<TrackViewPage>(allTracksListId(), *adapter, _layoutModel, _metadataCoordinator);

    auto pageId = pageNameForListId(allTracksListId());
    _stack.add(*trackPage, pageId, "All Tracks");

    TrackPageContext ctx;
    ctx.membershipList = nullptr;
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);

    bindTrackPage(ctx);
    _trackPages[allTracksListId()] = std::move(ctx);
  }

  void TrackPageGraph::buildPageForStoredList(ao::ListId listId,
                                              ao::library::ListView const& view,
                                              LibrarySession& session)
  {
    std::string listName = view.name().empty() ? "<Unnamed List>" : std::string(view.name());

    std::unique_ptr<ao::model::TrackIdList> membershipList;
    if (view.isSmart())
    {
      auto* sourceList = static_cast<ao::model::TrackIdList*>(session.allTrackIds.get());
      if (!view.isRootParent())
      {
        if (auto* const sourceCtx = find(view.parentId()))
        {
          if (sourceCtx->membershipList != nullptr)
          {
            sourceList = sourceCtx->membershipList.get();
          }
        }
      }

      auto filtered =
        std::make_unique<ao::model::FilteredTrackIdList>(*sourceList, *session.musicLibrary, *session.smartListEngine);
      filtered->setExpression(std::string(view.filter()));
      filtered->reload();
      membershipList = std::move(filtered);
    }
    else
    {
      auto manual = std::make_unique<ao::model::ManualTrackIdList>(view, session.allTrackIds.get());
      membershipList = std::move(manual);
    }

    auto adapter = std::make_unique<TrackListAdapter>(*membershipList, *session.musicLibrary, *session.rowDataProvider);
    adapter->onReset();

    auto trackPage = std::make_unique<TrackViewPage>(listId, *adapter, _layoutModel, _metadataCoordinator);
    auto pageId = pageNameForListId(listId);
    _stack.add(*trackPage, pageId, listName);

    auto playlistDir = session.musicLibrary->rootPath() / "playlist";

    if (!std::filesystem::exists(playlistDir))
    {
      std::filesystem::create_directories(playlistDir);
    }

    auto playlistPath = playlistDir / (listName + ".m3u");
    auto exporter = std::make_unique<ao::gtk::services::PlaylistExporter>(
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
    auto* const page = ctx.page.get();

    page->signalSelectionChanged().connect(
      [this, page]
      {
        if (_callbacks.onSelectionChanged != nullptr)
        {
          _callbacks.onSelectionChanged(page->getSelectedTrackIds());
        }
      });

    page->signalContextMenuRequested().connect(
      [this, page](double posX, double posY)
      {
        if (_callbacks.onContextMenuRequested != nullptr)
        {
          _callbacks.onContextMenuRequested(*page, posX, posY);
        }
      });

    page->signalTagEditRequested().connect(
      [this, page](std::vector<ao::TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (_callbacks.onTagEditRequested != nullptr)
        {
          _callbacks.onTagEditRequested(*page, ids, relativeTo);
        }
      });

    page->signalTrackActivated().connect(
      [this, page](ao::TrackId id)
      {
        if (_callbacks.onTrackActivated != nullptr)
        {
          _callbacks.onTrackActivated(*page, id);
        }
      });

    page->signalCreateSmartListRequested().connect(
      [this, page](std::string const& expression)
      {
        if (_callbacks.onCreateSmartListRequested != nullptr)
        {
          _callbacks.onCreateSmartListRequested(*page, expression);
        }
      });

    // Set initial playing track state
    if (_playingTrackId)
    {
      page->setPlayingTrackId(_playingTrackId);
    }
  }
} // namespace ao::gtk
