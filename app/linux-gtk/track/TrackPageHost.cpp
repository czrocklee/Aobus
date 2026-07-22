// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/Log.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/track/TrackPageRoute.h>

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
                               TagEditController& tagEditController,
                               ListNavigationController& listNavigation,
                               uimodel::TrackColumnLayoutStore& layoutStore)
    : _stack{stack}
    , _runtime{runtime}
    , _tagEditController{tagEditController}
    , _listNavigation{listNavigation}
    , _layoutStore{layoutStore}
  {
    _revealSub =
      _runtime.playback().events().onRevealTrackRequested(std::bind_front(&TrackPageHost::handleRevealTrack, this));

    auto& playback = _runtime.playback();
    setPlayingTrack(playback.snapshot().transport.nowPlaying.trackId);
    _snapshotSub = playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot)
                                                { setPlayingTrack(snapshot.transport.nowPlaying.trackId); });

    _focusSub = _runtime.workspace().onChanged(
      [this](rt::WorkspaceChanged const& changed)
      {
        if (changed.cause != rt::WorkspaceChangeCause::Presentation &&
            changed.cause != rt::WorkspaceChangeCause::Presets)
        {
          syncLayout();
        }
      });

    _viewDestroyedSub = _runtime.views().onDestroyed([this](auto) { syncLayout(); });

    _projectionChangedSub = _runtime.views().onProjectionChanged(
      [this](rt::TrackListProjectionChanged const& ev)
      {
        auto* entry = find(ev.viewId);

        if (entry == nullptr || entry->modelPtr == nullptr)
        {
          return;
        }

        entry->modelPtr->bindProjection(ev.projectionPtr);
        entry->pagePtr->applyPresentation(ev.projectionPtr->presentation());

        if (_playingTrackId != kInvalidTrackId)
        {
          entry->pagePtr->setPlayingTrackId(_playingTrackId);
        }
      });

    _presentationChangedSub = _runtime.views().onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev)
      {
        auto* entry = find(ev.viewId);

        if (entry == nullptr || entry->pagePtr == nullptr)
        {
          return;
        }

        entry->pagePtr->applyPresentation(ev.presentation);

        if (_playingTrackId != kInvalidTrackId)
        {
          entry->pagePtr->setPlayingTrackId(_playingTrackId);
        }
      });
  }
  rt::ViewId TrackPageHost::tryFindViewByPreferredList(ListId preferredListId)
  {
    for (auto const& [id, entry] : _trackPages)
    {
      if (entry.pagePtr && entry.pagePtr->listId() == preferredListId)
      {
        return id;
      }
    }

    return rt::kInvalidViewId;
  }

  void TrackPageHost::tryRevealTrackInView(rt::ViewId viewId, TrackId trackId)
  {
    if (auto const focused = _runtime.workspace().focusView(viewId); !focused)
    {
      APP_LOG_DEBUG("TrackPageHost: Could not focus view {} for reveal: {}", viewId.raw(), focused.error().message);
      return;
    }

    if (trackId == kInvalidTrackId)
    {
      return;
    }

    auto* const entry = find(viewId);

    if (entry == nullptr)
    {
      return;
    }

    entry->pagePtr->selectionController().selectTrack(trackId);
  }

  void TrackPageHost::handleRevealTrack(rt::PlaybackRevealTrackRequest const& ev)
  {
    auto viewId = rt::ViewId{ev.preferredViewId};

    if (viewId == rt::kInvalidViewId && ev.preferredListId != kInvalidListId)
    {
      viewId = tryFindViewByPreferredList(ev.preferredListId);
    }

    if (viewId != rt::kInvalidViewId)
    {
      if (find(viewId) == nullptr)
      {
        syncLayout();
      }

      tryRevealTrackInView(viewId, ev.trackId);
    }
  }

  void TrackPageHost::syncLayout()
  {
    if (_activeDataProvider == nullptr)
    {
      return;
    }

    auto const state = _runtime.workspace().snapshot();

    // Remove closed views
    for (auto it = _trackPages.begin(); it != _trackPages.end();)
    {
      if (!std::ranges::contains(state.openViews, it->first))
      {
        if (it->second.pagePtr)
        {
          _stack.remove(*it->second.pagePtr);
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

      if (it->second.pagePtr)
      {
        _stack.remove(*it->second.pagePtr);
      }

      _trackPages.erase(it);
    }
  }

  void TrackPageHost::rebuild(TrackRowCache& dataProvider)
  {
    APP_LOG_DEBUG("TrackPageHost::rebuild called");
    clear();
    _activeDataProvider = &dataProvider;

    // Force layout state re-evaluation now that dataProvider is ready
    auto const layout = _runtime.workspace().snapshot();

    for (auto const viewId : layout.openViews)
    {
      ensureViewPage(viewId, *_activeDataProvider);
    }

    if (layout.activeViewId != rt::kInvalidViewId)
    {
      _stack.set_visible_child(std::format("view-{}", layout.activeViewId.raw()));
    }
  }

  TrackPageEntry* TrackPageHost::find(rt::ViewId viewId)
  {
    auto it = _trackPages.find(viewId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageEntry const* TrackPageHost::find(rt::ViewId viewId) const
  {
    auto it = _trackPages.find(viewId);
    return (it != _trackPages.end()) ? &it->second : nullptr;
  }

  TrackPageEntry* TrackPageHost::currentVisible()
  {
    auto* const visibleChild = _stack.get_visible_child();

    if (visibleChild == nullptr)
    {
      return nullptr;
    }

    for (auto& [id, entry] : _trackPages)
    {
      if (entry.pagePtr.get() == visibleChild)
      {
        return &entry;
      }
    }

    return nullptr;
  }

  TrackPageEntry const* TrackPageHost::currentVisible() const
  {
    auto const* const visibleChild = _stack.get_visible_child();

    if (visibleChild == nullptr)
    {
      return nullptr;
    }

    for (auto const& [id, entry] : _trackPages)
    {
      if (entry.pagePtr.get() == visibleChild)
      {
        return &entry;
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

    for (auto& [id, entry] : _trackPages)
    {
      if (entry.pagePtr)
      {
        entry.pagePtr->setPlayingTrackId(trackId);
      }
    }
  }

  void TrackPageHost::ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider)
  {
    if (_trackPages.contains(viewId))
    {
      return; // Already exists
    }

    auto projPtr = _runtime.views().trackListProjection(viewId);

    if (!projPtr)
    {
      return;
    }

    auto const state = _runtime.views().trackListState(viewId);
    auto const listId = ListId{state.listId};

    auto modelPtr = TrackListModel::create(dataProvider);
    modelPtr->bindProjection(projPtr);

    auto trackPagePtr =
      std::make_unique<TrackViewPage>(listId, modelPtr, _layoutStore, _runtime, _thumbnailLoader, viewId);
    auto const pageId = std::format("view-{}", viewId.raw());

    auto listName = std::string{"List"};

    if (listId != rt::kAllTracksListId && listId != kInvalidListId)
    {
      auto scope = _runtime.library().reader();

      if (auto optNode = scope.listNode(listId); optNode)
      {
        listName = optNode->name.empty() ? "<Unnamed List>" : optNode->name;
      }
    }
    else if (listId == rt::kAllTracksListId)
    {
      listName = "All Tracks";
    }

    _stack.add(*trackPagePtr, pageId, listName);

    auto entry = TrackPageEntry{.viewId = viewId, .modelPtr = std::move(modelPtr), .pagePtr = std::move(trackPagePtr)};

    bindTrackPage(entry);
    _trackPages[viewId] = std::move(entry);
  }

  void TrackPageHost::bindTrackPage(TrackPageEntry& entry)
  {
    auto* const page = entry.pagePtr.get();
    auto const viewId = rt::ViewId{entry.viewId};

    page->signalSelectionChanged().connect([this, page, viewId] { handleTrackSelectionChanged(*page, viewId); });

    page->signalContextMenuRequested().connect(
      [this, page](double xPosition, double yPosition)
      {
        auto const sel =
          TrackSelection{.listId = page->listId(), .selectedIds = page->selectionController().selectedTrackIds()};
        _tagEditController.openTrackContextMenu(*page, sel, xPosition, yPosition);
      });

    page->signalTagEditRequested().connect(
      [this, page](std::vector<TrackId> const& ids, Gtk::Widget* relativeTo)
      {
        if (!relativeTo)
        {
          return;
        }

        auto const sel = TrackSelection{.listId = page->listId(), .selectedIds = ids};
        _tagEditController.openTagEditor(sel, *relativeTo);
      });

    page->signalTrackActivated().connect([this, viewId](TrackId id)
                                         { std::ignore = _runtime.playback().commands().startFromView(viewId, id); });

    page->signalCreateSmartListRequested().connect(
      [this, page](std::string const& expression)
      {
        auto const parentId = ao::uimodel::smartListParentIdFromPage(page->listId());
        _listNavigation.createSmartListFromExpression(parentId, expression);
      });

    // Set initial playing track state
    if (_playingTrackId != kInvalidTrackId)
    {
      page->setPlayingTrackId(_playingTrackId);
    }
  }

  void TrackPageHost::handleTrackSelectionChanged(TrackViewPage& page, rt::ViewId const viewId)
  {
    if (viewId == rt::kInvalidViewId)
    {
      return;
    }

    if (auto result = _runtime.views().setSelection(viewId, page.selectionController().selectedTrackIds()); !result)
    {
      APP_LOG_ERROR("Failed to publish track selection: {}", result.error().message);
    }

    if (auto const focused = _runtime.workspace().focusView(viewId); !focused)
    {
      APP_LOG_ERROR("Failed to focus selected track view: {}", focused.error().message);
    }
  }

  ListId TrackPageHost::activeListId() const
  {
    if (auto const* entry = currentVisible(); entry != nullptr)
    {
      return entry->pagePtr->listId();
    }

    return rt::kAllTracksListId;
  }
} // namespace ao::gtk
