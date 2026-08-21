// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
  class WorkspaceService;
} // namespace ao::rt

namespace ao::tui
{
  class TuiTextCatalog;
  class CommandCompletionProvider final
  {
  public:
    CommandCompletionProvider(rt::CompletionService& completion,
                              rt::WorkspaceService& workspace,
                              uimodel::PresentationTextCatalog textCatalog,
                              TuiTextCatalog const& tuiTextCatalog);

    std::optional<rt::CompletionResult> completeCommand(std::string_view draft);
    std::optional<rt::CompletionResult> completeFilter(std::string_view draft);

  private:
    rt::WorkspaceService& _workspace;
    uimodel::PresentationTextCatalog _textCatalog;
    TuiTextCatalog const& _tuiTextCatalog;
    uimodel::TrackFilterCompleter _filterCompleter;
  };
} // namespace ao::tui
