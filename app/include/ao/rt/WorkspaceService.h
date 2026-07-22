// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "TrackPresentation.h"
#include "ViewIds.h"
#include "ViewState.h"
#include "WorkspaceSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ao::async
{
  class Executor;
}

namespace ao::rt
{
  class ViewService;
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
    WorkspaceService(async::Executor& executor, ViewService& views, LibraryChanges const& changes);
    ~WorkspaceService();

    WorkspaceService(WorkspaceService const&) = delete;
    WorkspaceService& operator=(WorkspaceService const&) = delete;
    WorkspaceService(WorkspaceService&&) = delete;
    WorkspaceService& operator=(WorkspaceService&&) = delete;

    WorkspaceSnapshot snapshot() const;

    Result<> focusView(ViewId viewId);
    Result<ViewId> navigateTo(NavigationTarget const& target, NavigationOptions options = {});
    Result<> closeView(ViewId viewId);

    Result<> setActivePresentation(TrackPresentationSpec const& presentation, NavigationOptions options = {});
    Result<TrackPresentationSpec> setActivePresentation(std::string_view presentationId,
                                                        NavigationOptions options = {});

    Result<> goBack();
    Result<> goForward();

    async::Subscription onChanged(std::move_only_function<void(WorkspaceChanged const&)> handler);

    std::span<CustomTrackPresentationPreset const> customPresets() const;
    Result<> addCustomPreset(CustomTrackPresentationPreset const& preset);
    Result<> removeCustomPreset(std::string_view presetId);

    void saveSession(ConfigStore& store) const;
    Result<> restoreSession(ConfigStore& store);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
