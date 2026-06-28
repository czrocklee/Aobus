// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/track/TrackPresentationCatalog.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>
#include <ao/uimodel/track/TrackPresentationWorkflow.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::track
{
  TrackPresentationWorkflow::TrackPresentationWorkflow(
    rt::ViewService& views,
    rt::WorkspaceService& workspace,
    TrackPresentationCatalog& catalog,
    TrackPresentationPreferenceStore& preferences,
    std::function<void(TrackPresentationPickerState const&)> onRender)
    : _views{views}, _workspace{workspace}, _catalog{catalog}, _preferences{preferences}, _onRender{std::move(onRender)}
  {
    _focusSub = _workspace.onFocusedViewChanged(
      [this](rt::ViewId)
      {
        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _presentationSub = _views.onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev)
      {
        if (ev.viewId != _workspace.layoutState().activeViewId)
        {
          return;
        }

        _optimisticViewId = rt::kInvalidViewId;
        _optimisticPresentationId.clear();
        refresh();
      });

    _catalogSub = _catalog.signalChanged().connect([this] { refresh(); });
  }

  TrackPresentationPickerState TrackPresentationWorkflow::state() const
  {
    auto result = TrackPresentationPickerState{
      .enabled = false,
      .activeViewId = rt::kInvalidViewId,
      .activeListId = kInvalidListId,
      .activePresentationId = {},
      .label = "Presentation",
      .menuItems = _catalog.menuItems(),
    };
    auto const activeViewId = _workspace.layoutState().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return result;
    }

    auto const viewState = _views.trackListState(activeViewId);
    auto presentationId = viewState.presentation.id;

    if (_optimisticViewId == activeViewId && !_optimisticPresentationId.empty())
    {
      presentationId = _optimisticPresentationId;
    }

    result.enabled = true;
    result.activeViewId = activeViewId;
    result.activeListId = viewState.listId;
    result.activePresentationId = presentationId;
    result.label = _catalog.labelForId(presentationId);
    return result;
  }

  void TrackPresentationWorkflow::refresh()
  {
    if (_onRender)
    {
      _onRender(state());
    }
  }

  TrackPresentationApplyCommand TrackPresentationWorkflow::selectPresentation(std::string_view presentationId)
  {
    auto const activeViewId = _workspace.layoutState().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto const optSpec = _catalog.specForId(presentationId);

    if (!optSpec)
    {
      return {};
    }

    if (auto const viewState = _views.trackListState(activeViewId); viewState.listId != kInvalidListId)
    {
      _preferences.setPresentationIdForList(viewState.listId, optSpec->id);
    }

    _optimisticViewId = activeViewId;
    _optimisticPresentationId = optSpec->id;
    refresh();

    return TrackPresentationApplyCommand{.shouldApply = true, .spec = *optSpec};
  }
} // namespace ao::uimodel::track
