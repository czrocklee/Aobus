// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPageGraph.h"
#include "ListSidebarController.h"
#include "PlaybackController.h"
#include "TagEditController.h"
#include "TrackRowDataProvider.h"
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/model/AllTrackIdsList.h>
#include <ao/model/FilteredTrackIdList.h>
#include <ao/model/ManualTrackIdList.h>
#include <ao/utility/Log.h>
#include <runtime/CommandBus.h>
#include <runtime/EventTypes.h>

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

    ao::ListId rootParentId()
    {
      return ao::ListId{0};
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
                                 ao::app::AppSession& session,
                                 PlaybackController* playbackController,
                                 TagEditController& tagEditController,
                                 ListSidebarController& listSidebar,
                                 Callbacks callbacks)
    : _stack{stack}
    , _layoutModel{layoutModel}
    , _session{session}
    , _playbackController{playbackController}
    , _tagEditController{tagEditController}
    , _listSidebar{listSidebar}
    , _callbacks{std::move(callbacks)}
  {
    _revealSub = _session.events().subscribe<ao::app::RevealTrackRequested>(
      [this](ao::app::RevealTrackRequested const& ev)
      {
        if (ev.preferredListId != ao::ListId{})
        {
          show(ev.preferredListId);
          if (ev.trackId != ao::TrackId{})
          {
            if (auto* ctx = find(ev.preferredListId))
            {
              ctx->page->selectTrack(ev.trackId);
            }
          }
        }
      });

    _nowPlayingSub = _session.events().subscribe<ao::app::NowPlayingTrackChanged>(
      [this](ao::app::NowPlayingTrackChanged const& ev)
      { setPlayingTrack(ev.trackId != ao::TrackId{} ? std::optional{ev.trackId} : std::nullopt); });
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

      if (it->second.viewId != ao::app::ViewId{})
      {
        _session.commands().execute<ao::app::DestroyView>(ao::app::DestroyView{.viewId = it->second.viewId});
      }

      if (it->second.page)
      {
        _stack.remove(*it->second.page);
      }

      _trackPages.erase(it);
    }
  }

  void TrackPageGraph::rebuild(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction& txn)
  {
    APP_LOG_DEBUG("TrackPageGraph::rebuild called");
    clear();

    buildPageForAllTracks(dataProvider);

    auto reader = _session.musicLibrary().lists().reader(txn);
    for (auto const& [id, listView] : reader)
    {
      buildPageForStoredList(id, listView, dataProvider);
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
    if (auto* ctx = find(listId); ctx && ctx->viewId != ao::app::ViewId{})
    {
      _session.commands().execute<ao::app::SetFocusedView>(ao::app::SetFocusedView{.viewId = ctx->viewId});
    }
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

  void TrackPageGraph::buildPageForAllTracks(TrackRowDataProvider& dataProvider)
  {
    auto cvResult = _session.commands().execute<ao::app::CreateTrackListView>(
      ao::app::CreateTrackListView{.initial = {.listId = allTracksListId()}, .attached = true});
    auto const viewId = cvResult ? cvResult->viewId : ao::app::ViewId{};

    auto adapter = std::make_unique<TrackListAdapter>(_session.allTracks(), _session.musicLibrary(), dataProvider);
    adapter->onReset();
    if (viewId != ao::app::ViewId{})
    {
      if (auto proj = _session.views().trackListProjection(viewId))
      {
        adapter->bindProjection(*proj);
      }
    }

    auto trackPage =
      std::make_unique<TrackViewPage>(allTracksListId(), *adapter, _layoutModel, _session.commands(), viewId);

    auto pageId = pageNameForListId(allTracksListId());
    _stack.add(*trackPage, pageId, "All Tracks");

    TrackPageContext ctx;
    ctx.viewId = viewId;
    ctx.membershipList = nullptr;
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);

    bindTrackPage(ctx);
    _trackPages[allTracksListId()] = std::move(ctx);
  }

  void TrackPageGraph::buildPageForStoredList(ao::ListId listId,
                                              ao::library::ListView const& view,
                                              TrackRowDataProvider& dataProvider)
  {
    std::string listName = view.name().empty() ? "<Unnamed List>" : std::string(view.name());

    std::shared_ptr<ao::model::TrackIdList> membershipList;
    if (view.isSmart())
    {
      auto* sourceList = &_session.allTracks();
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

      auto filtered = std::make_shared<ao::model::FilteredTrackIdList>(
        *sourceList, _session.musicLibrary(), _session.smartListEngine());
      filtered->setExpression(std::string(view.filter()));
      filtered->reload();
      membershipList = std::move(filtered);
    }
    else
    {
      auto manual = std::make_shared<ao::model::ManualTrackIdList>(view, &_session.allTracks());
      membershipList = std::move(manual);
    }

    auto cvResult = _session.commands().execute<ao::app::CreateTrackListView>(
      ao::app::CreateTrackListView{.initial = {.listId = listId}, .attached = true, .source = membershipList});
    auto const viewId = cvResult ? cvResult->viewId : ao::app::ViewId{};

    auto adapter = std::make_unique<TrackListAdapter>(*membershipList, _session.musicLibrary(), dataProvider);
    adapter->onReset();
    if (viewId != ao::app::ViewId{})
    {
      if (auto proj = _session.views().trackListProjection(viewId))
      {
        adapter->bindProjection(*proj);
      }
    }

    auto trackPage = std::make_unique<TrackViewPage>(listId, *adapter, _layoutModel, _session.commands(), viewId);
    auto pageId = pageNameForListId(listId);
    _stack.add(*trackPage, pageId, listName);

    auto playlistDir = _session.musicLibrary().rootPath() / "playlist";

    if (!std::filesystem::exists(playlistDir))
    {
      std::filesystem::create_directories(playlistDir);
    }

    auto playlistPath = playlistDir / (listName + ".m3u");
    auto exporter = std::make_unique<ao::gtk::services::PlaylistExporter>(
      *membershipList, dataProvider, _session.musicLibrary().rootPath(), playlistPath);

    TrackPageContext ctx;
    ctx.viewId = viewId;
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
    auto const viewId = ctx.viewId;

    page->signalSelectionChanged().connect(
      [this, page, viewId]
      {
        auto const ids = page->getSelectedTrackIds();
        if (viewId != ao::app::ViewId{})
        {
          _session.commands().execute<ao::app::SetViewSelection>(
            ao::app::SetViewSelection{.viewId = viewId, .selection = ids});
          _session.commands().execute<ao::app::SetFocusedView>(ao::app::SetFocusedView{.viewId = viewId});
        }
        if (_callbacks.onSelectionChanged)
        {
          _callbacks.onSelectionChanged(ids);
        }
      });

    page->signalContextMenuRequested().connect(
      [this, page](double posX, double posY)
      {
        auto* const self = find(page->getListId());
        TrackSelectionContext sel{.listId = page->getListId(),
                                  .selectedIds = page->getSelectedTrackIds(),
                                  .membershipList = self ? self->membershipList.get() : nullptr};
        _tagEditController.showTrackContextMenu(*page, sel, posX, posY);
      });

    page->signalTagEditRequested().connect(
      [this, page](std::vector<ao::TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (!relativeTo)
        {
          return;
        }
        auto* const self = find(page->getListId());
        TrackSelectionContext sel{.listId = page->getListId(),
                                  .selectedIds = ids,
                                  .membershipList = self ? self->membershipList.get() : nullptr};
        _tagEditController.showTagEditor(sel, *relativeTo);
      });

    page->signalTrackActivated().connect(
      [this, page](ao::TrackId id)
      {
        if (_playbackController) _playbackController->playFromPage(*page, id);
      });

    page->signalCreateSmartListRequested().connect(
      [this, page](std::string const& expression)
      {
        auto parentId = page->getListId();
        if (parentId == allTracksListId())
        {
          parentId = rootParentId();
        }
        _listSidebar.createSmartListFromExpression(parentId, expression);
      });

    // Set initial playing track state
    if (_playingTrackId)
    {
      page->setPlayingTrackId(_playingTrackId);
    }
  }
} // namespace ao::gtk
