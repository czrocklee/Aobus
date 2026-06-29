// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "TrackPresentation.h"
#include "ViewState.h"
#include "WorkspaceViewState.h"
#include <ao/CoreIds.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class ViewService;
  class PlaybackService;
  class LibraryChanges;
  class ConfigStore;

  struct NavigationOptions final
  {
    bool recordHistory = true;
    std::optional<TrackPresentationSpec> optPresentation = std::nullopt;
  };

  struct FilteredListTarget
  {
    ListId listId;
    std::string filterExpression;
  };

  using NavigationTarget = std::variant<ListId, FilteredListTarget, GlobalViewKind>;

  class WorkspaceService final
  {
  public:
    struct NavigationHistoryChanged final
    {
      bool canGoBack = false;
      bool canGoForward = false;
    };

    WorkspaceService(ViewService& views,
                     PlaybackService& playback,
                     LibraryChanges const& changes,
                     library::MusicLibrary& library);
    ~WorkspaceService();

    WorkspaceService(WorkspaceService const&) = delete;
    WorkspaceService& operator=(WorkspaceService const&) = delete;
    WorkspaceService(WorkspaceService&&) = delete;
    WorkspaceService& operator=(WorkspaceService&&) = delete;

    WorkspaceViewState layoutState() const;

    void setFocusedView(ViewId viewId);
    void addView(ViewId viewId);
    void navigateTo(NavigationTarget const& target, NavigationOptions options = {});
    void closeView(ViewId viewId);

    void setActivePresentation(TrackPresentationSpec const& presentation, NavigationOptions options = {});
    TrackPresentationSpec setActivePresentation(std::string_view presentationId, NavigationOptions options = {});

    void jumpToAlbum(TrackId trackId);

    bool goBack();
    bool goForward();
    bool canGoBack() const noexcept;
    bool canGoForward() const noexcept;

    Subscription onFocusedViewChanged(std::move_only_function<void(ViewId)> handler);
    Subscription onNavigationHistoryChanged(std::move_only_function<void(NavigationHistoryChanged const&)> handler);

    std::span<CustomTrackPresentationPreset const> customPresets() const;
    void addCustomPreset(CustomTrackPresentationPreset const& preset);
    void removeCustomPreset(std::string_view presetId);
    void setCustomPresets(std::vector<CustomTrackPresentationPreset> presets);

    Subscription onCustomPresetsChanged(std::move_only_function<void()> handler);

    void saveSession(ConfigStore& store) const;
    void restoreSession(ConfigStore& store);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
