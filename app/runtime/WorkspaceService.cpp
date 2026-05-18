// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "WorkspaceService.h"

#include "LibraryMutationService.h"
#include "PlaybackService.h"
#include "ViewService.h"

#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <runtime/CorePrimitives.h>
#include <runtime/StateTypes.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  struct WorkspaceService::Impl final
  {
    ViewService& views;
    PlaybackService& playback;
    LibraryMutationService& mutation;
    library::MusicLibrary& library;
    LayoutState layoutState;
    Subscription listsMutatedSub;

    Signal<ViewId> focusedViewChangedSignal;

    Impl(WorkspaceService* self,
         ViewService& views,
         PlaybackService& playback,
         LibraryMutationService& mutation,
         library::MusicLibrary& library)
      : views{views}, playback{playback}, mutation{mutation}, library{library}
    {
      listsMutatedSub = mutation.onListsMutated(
        [this, self](LibraryMutationService::ListsMutated const& ev)
        {
          auto toClose = std::vector<ViewId>{};

          for (auto const id : ev.deleted)
          {
            for (auto const viewId : this->layoutState.openViews)
            {
              if (auto const& state = this->views.trackListState(viewId); state.listId == id)
              {
                toClose.push_back(viewId);
              }
            }
          }

          for (auto const viewId : toClose)
          {
            self->closeView(viewId);
          }
        });
    }
  };

  WorkspaceService::WorkspaceService(ViewService& views,
                                     PlaybackService& playback,
                                     LibraryMutationService& mutation,
                                     library::MusicLibrary& library)
    : _impl{std::make_unique<Impl>(this, views, playback, mutation, library)}
  {
  }

  WorkspaceService::~WorkspaceService() = default;

  Subscription WorkspaceService::onFocusedViewChanged(std::move_only_function<void(ViewId)> handler)
  {
    return _impl->focusedViewChangedSignal.connect(std::move(handler));
  }

  LayoutState WorkspaceService::layoutState() const
  {
    return _impl->layoutState;
  }

  void WorkspaceService::setFocusedView(ViewId const viewId)
  {
    _impl->layoutState.activeViewId = viewId;
    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(viewId);
  }

  void WorkspaceService::addView(ViewId const viewId)
  {
    if (std::ranges::find(_impl->layoutState.openViews, viewId) == _impl->layoutState.openViews.end())
    {
      _impl->layoutState.openViews.push_back(viewId);
      _impl->layoutState.revision++;
    }
  }

  void WorkspaceService::navigateTo(std::variant<ListId, std::string, GlobalViewKind> const& target)
  {
    auto targetViewId = ViewId{};

    if (std::holds_alternative<ListId>(target))
    {
      auto const listId = ListId{std::get<ListId>(target)};

      for (auto const& record : _impl->views.listViews())
      {
        if (record.kind == ViewKind::TrackList)
        {
          if (auto const& state = _impl->views.trackListState(record.id);
              state.listId == listId && state.filterExpression.empty())
          {
            targetViewId = record.id;
            break;
          }
        }
      }

      if (targetViewId == ViewId{})
      {
        auto const res = _impl->views.createView(TrackListViewConfig{.listId = listId}, true);
        targetViewId = res.viewId;
      }
    }
    else if (std::holds_alternative<std::string>(target))
    {
      auto const query = std::get<std::string>(target);
      auto const res = _impl->views.createView(TrackListViewConfig{.filterExpression = query}, true);
      targetViewId = res.viewId;
    }
    else if (std::holds_alternative<GlobalViewKind>(target))
    {
      if (auto const kind = std::get<GlobalViewKind>(target); kind == GlobalViewKind::AllTracks)
      {
        auto const res = _impl->views.createView(TrackListViewConfig{}, true);
        targetViewId = res.viewId;
      }
    }

    if (targetViewId != ViewId{})
    {
      if (std::ranges::find(_impl->layoutState.openViews, targetViewId) == _impl->layoutState.openViews.end())
      {
        _impl->layoutState.openViews.push_back(targetViewId);
      }

      _impl->layoutState.activeViewId = targetViewId;
      _impl->layoutState.revision++;
      _impl->focusedViewChangedSignal.emit(targetViewId);
    }
  }

  void WorkspaceService::closeView(ViewId const viewId)
  {
    if (auto const it = std::ranges::find(_impl->layoutState.openViews, viewId);
        it != _impl->layoutState.openViews.end())
    {
      _impl->layoutState.openViews.erase(it);
    }

    if (_impl->layoutState.activeViewId == viewId)
    {
      _impl->layoutState.activeViewId =
        _impl->layoutState.openViews.empty() ? ViewId{} : _impl->layoutState.openViews.back();
    }

    _impl->layoutState.revision++;
    _impl->focusedViewChangedSignal.emit(_impl->layoutState.activeViewId);

    _impl->views.destroyView(viewId);
  }
} // namespace ao::rt
