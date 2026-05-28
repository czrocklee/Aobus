// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "track/TrackListModel.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/Type.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ManualListSource.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/track/TrackPageRoute.h>
#include <ao/utility/Log.h>

#include <gtkmm/stack.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  TrackPageHost::TrackPageHost(Gtk::Stack& stack,
                               rt::AppRuntime& runtime,
                               ao::uimodel::playback::PlaybackQueueModel* queueModel,
                               TagEditController& tagEditController,
                               ListNavigationController& listSidebar,
                               TrackPresentationStore& presentationStore,
                               ImageCache* imageCache)
    : _stack{stack}
    , _runtime{runtime}
    , _playbackQueueModel{queueModel}
    , _tagEditController{tagEditController}
    , _listSidebar{listSidebar}
    , _presentationStore{presentationStore}
    , _imageCache{imageCache}
  {
    _revealSub = _runtime.playback().onRevealTrackRequested(std::bind_front(&TrackPageHost::handleRevealTrack, this));

    _nowPlayingSub = _runtime.playback().onNowPlayingChanged([this](auto const& ev) { setPlayingTrack(ev.trackId); });

    _focusSub = _runtime.workspace().onFocusedViewChanged([this](auto) { syncLayout(); });

    _viewDestroyedSub = _runtime.views().onDestroyed([this](auto) { syncLayout(); });

    _projectionChangedSub = _runtime.views().onProjectionChanged(
      [this](rt::TrackListProjectionChanged const& ev)
      {
        auto* ctx = find(ev.viewId);

        if (ctx == nullptr || ctx->model == nullptr)
        {
          return;
        }

        ctx->model->bindProjection(ev.projection);
        ctx->page->applyPresentation(ev.projection->presentation());

        if (_playingTrackId != kInvalidTrackId)
        {
          ctx->page->setPlayingTrackId(_playingTrackId);
        }
      });

    _presentationChangedSub = _runtime.views().onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev)
      {
        auto* ctx = find(ev.viewId);

        if (ctx == nullptr || ctx->page == nullptr)
        {
          return;
        }

        ctx->page->applyPresentation(ev.presentation);

        if (_playingTrackId != kInvalidTrackId)
        {
          ctx->page->setPlayingTrackId(_playingTrackId);
        }
      });
  }
  rt::ViewId TrackPageHost::tryFindViewByPreferredList(ListId preferredListId)
  {
    for (auto const& [id, ctx] : _trackPages)
    {
      if (ctx.page && ctx.page->listId() == preferredListId)
      {
        return id;
      }
    }

    return rt::kInvalidViewId;
  }

  void TrackPageHost::tryRevealTrackInView(rt::ViewId viewId, TrackId trackId)
  {
    APP_LOG_DEBUG("TrackPageHost: Revealing in viewId: {}", viewId.raw());
    _runtime.workspace().setFocusedView(viewId);

    if (trackId != kInvalidTrackId)
    {
      if (auto* ctx = find(viewId))
      {
        APP_LOG_DEBUG("TrackPageHost: Calling selectTrack on page for trackId: {}", trackId.raw());
        ctx->page->selectionController().selectTrack(trackId);
      }
      else
      {
        APP_LOG_DEBUG("TrackPageHost: Could not find page context for viewId: {}", viewId.raw());
      }
    }
  }

  void TrackPageHost::handleRevealTrack(rt::PlaybackService::RevealTrackRequested const& ev)
  {
    auto viewId = rt::ViewId{ev.preferredViewId};

    if (viewId == rt::kInvalidViewId && ev.preferredListId != kInvalidListId)
    {
      viewId = tryFindViewByPreferredList(ev.preferredListId);
    }

    if (viewId != rt::kInvalidViewId)
    {
      tryRevealTrackInView(viewId, ev.trackId);
    }
  }

  void TrackPageHost::syncLayout()
  {
    if (_activeDataProvider == nullptr)
    {
      return;
    }

    auto const state = _runtime.workspace().layoutState();

    // Remove closed views
    for (auto it = _trackPages.begin(); it != _trackPages.end();)
    {
      if (!std::ranges::contains(state.openViews, it->first))
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
    if (state.activeViewId != rt::kInvalidViewId)
    {
      _stack.set_visible_child(std::format("view-{}", state.activeViewId.raw()));
    }
  }

  TrackPageHost::~TrackPageHost()
  {
    clear();
  }

  void TrackPageHost::clear()
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

  void TrackPageHost::rebuild(TrackRowCache& dataProvider, lmdb::ReadTransaction const& /*txn*/)
  {
    APP_LOG_DEBUG("TrackPageHost::rebuild called");
    clear();
    _activeDataProvider = &dataProvider;

    // Force layout state re-evaluation now that dataProvider is ready
    auto const layout = _runtime.workspace().layoutState();

    for (auto const viewId : layout.openViews)
    {
      ensureViewPage(viewId, *_activeDataProvider);
    }

    if (layout.activeViewId != rt::kInvalidViewId)
    {
      _stack.set_visible_child(std::format("view-{}", layout.activeViewId.raw()));
    }
  }

  TrackPageContext* TrackPageHost::find(rt::ViewId viewId)
  {
    auto it = _trackPages.find(viewId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext const* TrackPageHost::find(rt::ViewId viewId) const
  {
    auto it = _trackPages.find(viewId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageContext* TrackPageHost::currentVisible()
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

  TrackPageContext const* TrackPageHost::currentVisible() const
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

  void TrackPageHost::setPlayingTrack(TrackId trackId)
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

  void TrackPageHost::ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider)
  {
    if (_trackPages.contains(viewId))
    {
      return; // Already exists
    }

    auto proj = _runtime.views().trackListProjection(viewId);

    if (!proj)
    {
      return;
    }

    auto const state = _runtime.views().trackListState(viewId);
    auto const listId = ListId{state.listId};

    auto model = TrackListModel::create(dataProvider);
    model->bindProjection(proj);

    auto trackPage = std::make_unique<TrackViewPage>(listId, model, _presentationStore, _runtime, *_imageCache, viewId);
    auto const pageId = std::format("view-{}", viewId.raw());

    auto listName = std::string{"List"};

    if (listId != rt::kAllTracksListId && listId != kInvalidListId)
    {
      auto const txn = _runtime.musicLibrary().readTransaction();
      auto lists = _runtime.musicLibrary().lists().reader(txn);

      if (auto optView = lists.get(listId))
      {
        listName = optView->name().empty() ? "<Unnamed List>" : std::string{optView->name()};
      }
    }
    else if (listId == rt::kAllTracksListId)
    {
      listName = "All Tracks";
    }

    _stack.add(*trackPage, pageId, listName);

    auto ctx = TrackPageContext{.viewId = viewId, .model = std::move(model), .page = std::move(trackPage)};

    bindTrackPage(ctx);
    _trackPages[viewId] = std::move(ctx);
  }

  void TrackPageHost::bindTrackPage(TrackPageContext& ctx)
  {
    auto* const page = ctx.page.get();
    auto const viewId = rt::ViewId{ctx.viewId};

    page->signalSelectionChanged().connect(
      [this, page, viewId]
      {
        auto const route =
          ao::uimodel::track::describeSelectionRoute(viewId, page->selectionController().selectedTrackIds());

        if (route.shouldUpdateRuntimeSelection)
        {
          _runtime.views().setSelection(route.focusedViewId, route.selectedIds);
          _runtime.workspace().setFocusedView(route.focusedViewId);
        }
      });

    page->signalContextMenuRequested().connect(
      [this, page](double posX, double posY)
      {
        auto const sel = TrackSelectionContext{
          .listId = page->listId(), .selectedIds = page->selectionController().selectedTrackIds()};
        _tagEditController.showTrackContextMenu(*page, sel, posX, posY);
      });

    page->signalTagEditRequested().connect(
      [this, page](std::vector<TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (!relativeTo)
        {
          return;
        }

        auto const sel = TrackSelectionContext{.listId = page->listId(), .selectedIds = ids};
        _tagEditController.showTagEditor(sel, *relativeTo);
      });

    page->signalTrackActivated().connect(
      [this, page](TrackId id)
      {
        if (_playbackQueueModel)
        {
          _playbackQueueModel->playQueue(page->selectionController().visibleTrackIds(), id, page->listId());
        }
      });

    page->signalCreateSmartListRequested().connect(
      [this, page](std::string const& expression)
      {
        auto const parentId = ao::uimodel::track::smartListParentIdFromPage(page->listId());
        _listSidebar.createSmartListFromExpression(parentId, expression);
      });

    // Set initial playing track state
    if (_playingTrackId != kInvalidTrackId)
    {
      page->setPlayingTrackId(_playingTrackId);
    }
  }

  ListId TrackPageHost::activeListId() const
  {
    if (auto const* ctx = currentVisible())
    {
      return ctx->page->listId();
    }

    return rt::kAllTracksListId;
  }
} // namespace ao::gtk
