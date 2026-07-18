// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class ViewService;
  class WorkspaceService;
}

namespace ao::uimodel
{
  class ListPresentationPreferenceStore;

  struct TrackPresentationPickerState final
  {
    bool enabled = false;
    rt::ViewId activeViewId = rt::kInvalidViewId;
    ListId activeListId = kInvalidListId;
    std::string activePresentationId = {};
    std::string label = "Presentation";
    std::vector<TrackPresentationMenuItem> menuItems;

    bool operator==(TrackPresentationPickerState const&) const = default;
  };

  struct TrackPresentationApplyCommand final
  {
    bool shouldApply = false;
    rt::TrackPresentationSpec spec;
  };

  class TrackPresentationPickerViewModel final
  {
  public:
    TrackPresentationPickerViewModel(rt::ViewService& views,
                                     rt::WorkspaceService& workspace,
                                     TrackPresentationCatalog& catalog,
                                     ListPresentationPreferenceStore& preferences,
                                     std::function<void(TrackPresentationPickerState const&)> onRender);
    ~TrackPresentationPickerViewModel() = default;

    TrackPresentationPickerViewModel(TrackPresentationPickerViewModel const&) = delete;
    TrackPresentationPickerViewModel& operator=(TrackPresentationPickerViewModel const&) = delete;
    TrackPresentationPickerViewModel(TrackPresentationPickerViewModel&&) = delete;
    TrackPresentationPickerViewModel& operator=(TrackPresentationPickerViewModel&&) = delete;

    TrackPresentationPickerState state() const;
    void refresh();
    TrackPresentationApplyCommand selectPresentation(std::string_view presentationId);

  private:
    rt::ViewService& _views;
    rt::WorkspaceService& _workspace;
    TrackPresentationCatalog& _catalog;
    ListPresentationPreferenceStore& _preferences;
    std::function<void(TrackPresentationPickerState const&)> _onRender;
    async::Subscription _focusSub;
    async::Subscription _presentationSub;
    async::Subscription _catalogSub;
    rt::ViewId _observedViewId = rt::kInvalidViewId;
    rt::ViewId _optimisticViewId = rt::kInvalidViewId;
    std::string _optimisticPresentationId;
  };
} // namespace ao::uimodel
