// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>

#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
  class WorkspaceService;
} // namespace ao::rt

namespace ao::tui
{
  class LibraryController;

  class CommandCompletionProvider final
  {
  public:
    CommandCompletionProvider(LibraryController& library,
                              rt::CompletionService& completion,
                              rt::WorkspaceService& workspace);

    std::optional<rt::CompletionResult> complete(std::string_view draft);

  private:
    LibraryController& _library;
    rt::CompletionService& _completion;
    rt::WorkspaceService& _workspace;
    rt::QueryExpressionCompleter _expressionCompleter;
  };
} // namespace ao::tui
