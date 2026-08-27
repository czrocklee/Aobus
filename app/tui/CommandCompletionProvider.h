// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
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
    CommandCompletionProvider(rt::CompletionService& completion,
                              rt::WorkspaceService& workspace,
                              i18n::MessageCatalog textCatalog);

    std::optional<rt::CompletionResult> completeCommand(std::string_view draft);
    std::optional<rt::CompletionResult> completeFilter(std::string_view draft);

  private:
    rt::WorkspaceService& _workspace;
    i18n::MessageCatalog _textCatalog;
    uimodel::TrackFilterCompleter _filterCompleter;
  };
} // namespace ao::tui
