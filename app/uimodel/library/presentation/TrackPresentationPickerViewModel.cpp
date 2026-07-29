// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  TrackPresentationEligibility trackPresentationEligibility(ListId const listId, std::string_view const presentationId)
  {
    if (listId == rt::kAllTracksListId && presentationId == rt::kListOrderTrackPresentationId)
    {
      return {
        .enabled = false,
        .disabledReason = "All Tracks has no saved order. Create a saved List to arrange the full library manually.",
      };
    }

    return {};
  }

  TrackPresentationPickerViewModel::TrackPresentationPickerViewModel(
    rt::ViewService& views,
    rt::WorkspaceService& workspace,
    TrackPresentationCatalog& catalog,
    ListPresentationPreferenceStore& preferences,
    std::function<void(TrackPresentationPickerState const&)> onRender)
    : _views{views}, _workspace{workspace}, _catalog{catalog}, _preferences{preferences}, _onRender{std::move(onRender)}
  {
    _observedViewId = _workspace.snapshot().activeViewId;
    _focusSub = _workspace.onChanged(
      [this](rt::WorkspaceChanged const& changed) noexcept
      {
        if (changed.snapshot.activeViewId == _observedViewId)
        {
          return;
        }

        _observedViewId = changed.snapshot.activeViewId;
        refresh();
      });

    _presentationSub = _views.onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const& ev) noexcept
      {
        if (ev.viewId != _workspace.snapshot().activeViewId)
        {
          return;
        }

        refresh();
      });

    _catalogSub = _catalog.signalChanged().connect([this] noexcept { refresh(); });
  }

  TrackPresentationPickerState TrackPresentationPickerViewModel::state() const
  {
    auto result = TrackPresentationPickerState{
      .enabled = false,
      .activeViewId = rt::kInvalidViewId,
      .label = "Presentation",
      .menuItems = _catalog.menuItems(),
    };
    auto const activeViewId = _workspace.snapshot().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return result;
    }

    // Reached from three observers via refresh(), so the active view may already
    // be gone by the time the notification is delivered.
    auto const foundState = _views.findTrackListState(activeViewId);

    if (!foundState)
    {
      return result;
    }

    result.enabled = true;
    result.activeViewId = activeViewId;
    result.label = _catalog.labelForId(foundState->presentation.id);

    for (auto& item : result.menuItems)
    {
      if (item.type != TrackPresentationMenuItemType::Preset)
      {
        continue;
      }

      auto eligibility = trackPresentationEligibility(foundState->listId, item.id);
      item.enabled = eligibility.enabled;
      item.disabledReason = std::move(eligibility.disabledReason);
    }

    return result;
  }

  void TrackPresentationPickerViewModel::refresh()
  {
    if (_onRender)
    {
      _onRender(state());
    }
  }

  std::optional<TrackPresentationSelection> TrackPresentationPickerViewModel::selectPresentation(
    std::string_view presentationId)
  {
    auto const activeViewId = _workspace.snapshot().activeViewId;

    if (activeViewId == rt::kInvalidViewId)
    {
      return {};
    }

    auto const foundState = _views.findTrackListState(activeViewId);

    if (!foundState)
    {
      return {};
    }

    if (!trackPresentationEligibility(foundState->listId, presentationId).enabled)
    {
      return {};
    }

    auto optSpec = _catalog.specForId(presentationId);

    if (!optSpec)
    {
      return {};
    }

    return TrackPresentationSelection{
      .targetViewId = activeViewId, .targetListId = foundState->listId, .spec = std::move(*optSpec)};
  }

  void TrackPresentationPickerViewModel::completeSelection(TrackPresentationSelection const& selection)
  {
    if (selection.targetListId == kInvalidListId)
    {
      return;
    }

    _preferences.setPresentationIdForList(selection.targetListId, selection.spec.id);
  }
} // namespace ao::uimodel
