// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackPageGraph.h"
#include "ListSidebarController.h"
#include "PlaybackController.h"
#include "TagEditController.h"
#include "TrackRowDataProvider.h"
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/utility/Log.h>
#include <runtime/AllTracksSource.h>
#include <runtime/AppSession.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/ManualListSource.h>
#include <runtime/PlaybackService.h>
#include <runtime/SmartListSource.h>
#include <runtime/StateTypes.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

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
        if (ev.preferredViewId != ao::app::ViewId{})
        {
          _session.workspace().setFocusedView(ev.preferredViewId);
          if (ev.trackId != ao::TrackId{})
          {
            if (auto* ctx = find(ev.preferredViewId))
            {
              ctx->page->selectTrack(ev.trackId);
            }
          }
        }
      });

    _nowPlayingSub = _session.events().subscribe<ao::app::NowPlayingTrackChanged>(
      [this](ao::app::NowPlayingTrackChanged const& ev)
      { setPlayingTrack(ev.trackId != ao::TrackId{} ? std::optional{ev.trackId} : std::nullopt); });

    auto const syncLayout = [this]
    {
      if (!_activeDataProvider)
      {
        return;
      }

      auto const state = _session.workspace().layoutState();

      // Remove closed views
      for (auto it = _trackPages.begin(); it != _trackPages.end();)
      {
        if (std::ranges::find(state.openViews, it->first) == state.openViews.end())
        {
          if (it->second.page)
          {
            _stack.remove(*it->second.page);
          }
          it = _trackPages.erase(it);
        }
        else
        {
          ++it;
        }
      }

      // Add new views
      for (auto const viewId : state.openViews)
      {
        ensureViewPage(viewId, *_activeDataProvider);
      }

      // Set active
      if (state.activeViewId != ao::app::ViewId{})
      {
        _stack.set_visible_child(std::format("view-{}", state.activeViewId.value()));
      }
    };

    _focusSub = _session.events().subscribe<ao::app::FocusedViewChanged>(
      [syncLayout](ao::app::FocusedViewChanged const&) { syncLayout(); });

    _viewDestroyedSub = _session.events().subscribe<ao::app::ViewDestroyed>([syncLayout](ao::app::ViewDestroyed const&)
                                                                            { syncLayout(); });
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

  void TrackPageGraph::rebuild(TrackRowDataProvider& dataProvider, ao::lmdb::ReadTransaction& /*txn*/)
  {
    APP_LOG_DEBUG("TrackPageGraph::rebuild called");
    clear();
    _activeDataProvider = &dataProvider;

    // Force layout state re-evaluation now that dataProvider is ready
    auto layout = _session.workspace().layoutState();
    for (auto const viewId : layout.openViews)
    {
      ensureViewPage(viewId, *_activeDataProvider);
    }
    if (layout.activeViewId != ao::app::ViewId{})
    {
      _stack.set_visible_child(std::format("view-{}", layout.activeViewId.value()));
    }
  }

  TrackPageContext* TrackPageGraph::find(ao::app::ViewId viewId)
  {
    auto it = _trackPages.find(viewId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext const* TrackPageGraph::find(ao::app::ViewId viewId) const
  {
    auto it = _trackPages.find(viewId);
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

  void TrackPageGraph::setPlayingTrack(std::optional<ao::TrackId> trackId)
  {
    if (_optPlayingTrackId == trackId)
    {
      return;
    }

    _optPlayingTrackId = trackId;

    for (auto& [id, ctx] : _trackPages)
    {
      if (ctx.page)
      {
        ctx.page->setPlayingTrackId(trackId);
      }
    }
  }

  void TrackPageGraph::ensureViewPage(ao::app::ViewId viewId, TrackRowDataProvider& dataProvider)
  {
    if (_trackPages.contains(viewId))
    {
      return; // Already exists
    }

    auto proj = _session.views().trackListProjection(viewId);
    if (!proj)
    {
      return;
    }

    auto const state = _session.views().trackListState(viewId);
    auto const listId = state.listId;

    // Provide dummy source, bindProjection takes over
    auto adapter =
      std::make_unique<TrackListAdapter>(_session.sources().allTracks(), _session.musicLibrary(), dataProvider);
    adapter->bindProjection(*proj);

    auto trackPage = std::make_unique<TrackViewPage>(listId, *adapter, _layoutModel, _session, viewId);
    auto pageId = std::format("view-{}", viewId.value());

    std::string listName = "List";
    if (listId != allTracksListId() && listId != ao::ListId{})
    {
      auto txn = _session.musicLibrary().readTransaction();
      auto lists = _session.musicLibrary().lists().reader(txn);
      if (auto optView = lists.get(listId))
      {
        listName = optView->name().empty() ? "<Unnamed List>" : std::string(optView->name());
      }
    }
    else if (listId == allTracksListId())
    {
      listName = "All Tracks";
    }

    _stack.add(*trackPage, pageId, listName);

    TrackPageContext ctx;
    ctx.viewId = viewId;
    ctx.membershipList = nullptr;
    ctx.adapter = std::move(adapter);
    ctx.page = std::move(trackPage);

    bindTrackPage(ctx);
    _trackPages[viewId] = std::move(ctx);
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
          _session.views().setSelection(viewId, ids);
          _session.workspace().setFocusedView(viewId);
        }
      });

    page->signalContextMenuRequested().connect(
      [this, page, viewId](double posX, double posY)
      {
        auto* const self = find(viewId);
        TrackSelectionContext sel{.listId = page->getListId(),
                                  .selectedIds = page->getSelectedTrackIds(),
                                  .membershipList = self ? self->membershipList.get() : nullptr};
        _tagEditController.showTrackContextMenu(*page, sel, posX, posY);
      });

    page->signalTagEditRequested().connect(
      [this, page, viewId](std::vector<ao::TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (!relativeTo)
        {
          return;
        }
        auto* const self = find(viewId);
        TrackSelectionContext sel{.listId = page->getListId(),
                                  .selectedIds = ids,
                                  .membershipList = self ? self->membershipList.get() : nullptr};
        _tagEditController.showTagEditor(sel, *relativeTo);
      });

    page->signalTrackActivated().connect(
      [this, page](ao::TrackId id)
      {
        if (_playbackController)
        {
          _playbackController->playFromPage(*page, id);
        }
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
    if (_optPlayingTrackId)
    {
      page->setPlayingTrackId(_optPlayingTrackId);
    }
  }
} // namespace ao::gtk
