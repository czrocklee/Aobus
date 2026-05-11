// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include <memory>
#include <string>
#include <variant>

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
                     ao::library::MusicLibrary& library,
                     std::shared_ptr<ConfigStore> configStore);
    ~WorkspaceService();

    WorkspaceService(WorkspaceService const&) = delete;
    WorkspaceService& operator=(WorkspaceService const&) = delete;
    WorkspaceService(WorkspaceService&&) = delete;
    WorkspaceService& operator=(WorkspaceService&&) = delete;

    LayoutState layoutState() const;

    void setFocusedView(ViewId viewId);
    void navigateTo(std::variant<ao::ListId, std::string, GlobalViewKind> target);
    void closeView(ViewId viewId);
    void restoreSession();
    void saveSession();

    Subscription onFocusedViewChanged(std::move_only_function<void(ViewId)> handler);
    Subscription onSessionRestored(std::move_only_function<void(std::string const&)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
