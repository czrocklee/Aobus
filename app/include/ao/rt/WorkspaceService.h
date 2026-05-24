// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include "TrackPresentation.h"
#include "ao/Type.h"

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

  class WorkspaceService final
  {
  public:
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
    void navigateTo(std::variant<ListId, std::string, GlobalViewKind> const& target);
    void closeView(ViewId viewId);

    Subscription onFocusedViewChanged(std::move_only_function<void(ViewId)> handler);

    std::span<CustomTrackPresentationPreset const> customPresets() const;
    void addCustomPreset(CustomTrackPresentationPreset const& preset);
    void removeCustomPreset(std::string_view presetId);
    void setCustomPresets(std::vector<CustomTrackPresentationPreset> presets);

    Subscription onCustomPresetsChanged(std::move_only_function<void()> handler);

    void saveSession(ConfigStore& store) const;
    void restoreSession(ConfigStore& store);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
