// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Subscription.h"
#include "TrackPresentation.h"
#include "ViewIds.h"
#include "ViewState.h"
#include "WorkspaceSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

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

  struct WorkspacePresentationReceipt final
  {
    TrackPresentationSpec presentation{};
    WorkspaceCommitReceipt commit{};
  };

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

    Result<WorkspaceCommitReceipt> focusView(ViewId viewId);
    Result<WorkspaceCommitReceipt> navigateTo(NavigationTarget const& target, NavigationOptions options = {});
    Result<WorkspaceCommitReceipt> closeView(ViewId viewId);

    Result<WorkspaceCommitReceipt> setActivePresentation(TrackPresentationSpec const& presentation,
                                                         NavigationOptions options = {});
    Result<WorkspacePresentationReceipt> setActivePresentation(std::string_view presentationId,
                                                               NavigationOptions options = {});

    Result<WorkspaceCommitReceipt> goBack();
    Result<WorkspaceCommitReceipt> goForward();
    bool canGoBack() const;
    bool canGoForward() const;

    Subscription onChanged(std::move_only_function<void(WorkspaceChanged const&)> handler);

    std::span<CustomTrackPresentationPreset const> customPresets() const;
    Result<WorkspaceCommitReceipt> addCustomPreset(CustomTrackPresentationPreset const& preset);
    Result<WorkspaceCommitReceipt> removeCustomPreset(std::string_view presetId);

    void saveSession(ConfigStore& store) const;
    Result<WorkspaceCommitReceipt> restoreSession(ConfigStore& store);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
