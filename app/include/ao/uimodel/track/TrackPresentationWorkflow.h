// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackPresentationCatalog.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class ViewService;
  class WorkspaceService;
}

namespace ao::uimodel::track
{
  class TrackPresentationPreferenceStore;

  struct TrackPresentationPickerState final
  {
    bool enabled = false;
    rt::ViewId activeViewId = rt::kInvalidViewId;
    ListId activeListId = kInvalidListId;
    std::string activePresentationId;
    std::string label = "Presentation";
    std::vector<TrackPresentationMenuItem> menuItems;

    bool operator==(TrackPresentationPickerState const&) const = default;
  };

  struct TrackPresentationApplyCommand final
  {
    bool shouldApply = false;
    rt::TrackPresentationSpec spec;
  };

  class TrackPresentationWorkflow final
  {
  public:
    TrackPresentationWorkflow(rt::ViewService& views,
                              rt::WorkspaceService& workspace,
                              TrackPresentationCatalog& catalog,
                              TrackPresentationPreferenceStore& preferences,
                              std::function<void(TrackPresentationPickerState const&)> onRender);
    ~TrackPresentationWorkflow() = default;

    TrackPresentationWorkflow(TrackPresentationWorkflow const&) = delete;
    TrackPresentationWorkflow& operator=(TrackPresentationWorkflow const&) = delete;
    TrackPresentationWorkflow(TrackPresentationWorkflow&&) = delete;
    TrackPresentationWorkflow& operator=(TrackPresentationWorkflow&&) = delete;

    TrackPresentationPickerState state() const;
    void refresh();
    TrackPresentationApplyCommand selectPresentation(std::string_view presentationId);

  private:
    rt::ViewService& _views;
    rt::WorkspaceService& _workspace;
    TrackPresentationCatalog& _catalog;
    TrackPresentationPreferenceStore& _preferences;
    std::function<void(TrackPresentationPickerState const&)> _onRender;
    rt::Subscription _focusSub;
    rt::Subscription _presentationSub;
    rt::Subscription _catalogSub;
    rt::ViewId _optimisticViewId = rt::kInvalidViewId;
    std::string _optimisticPresentationId;
  };
} // namespace ao::uimodel::track
