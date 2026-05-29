// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include "TrackPresentation.h"
#include <ao/Type.h>

#include <functional>
#include <memory>
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
  class LibraryMutationService;
  class ConfigStore;

  struct NavigationOptions final
  {
    bool recordHistory = true;
  };

  using NavigationTarget = std::variant<ListId, std::string, GlobalViewKind>;

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
                     LibraryMutationService& mutation,
                     library::MusicLibrary& library);
    ~WorkspaceService();

    WorkspaceService(WorkspaceService const&) = delete;
    WorkspaceService& operator=(WorkspaceService const&) = delete;
    WorkspaceService(WorkspaceService&&) = delete;
    WorkspaceService& operator=(WorkspaceService&&) = delete;

    LayoutState layoutState() const;

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
}
