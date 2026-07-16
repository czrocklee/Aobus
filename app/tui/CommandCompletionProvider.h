// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>

#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
  class WorkspaceService;
} // namespace ao::rt

namespace ao::tui
{
  class CommandCompletionProvider final
  {
  public:
    CommandCompletionProvider(rt::CompletionService& completion, rt::WorkspaceService& workspace);

    std::optional<rt::CompletionResult> complete(std::string_view draft);

  private:
    rt::WorkspaceService& _workspace;
    uimodel::TrackFilterCompleter _filterCompleter;
  };
} // namespace ao::tui
