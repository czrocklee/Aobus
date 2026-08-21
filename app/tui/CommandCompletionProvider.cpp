// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletionProvider.h"

#include "CommandCompletion.h"
#include "TuiTextCatalog.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::tui
{
  CommandCompletionProvider::CommandCompletionProvider(rt::CompletionService& completion,
                                                       rt::WorkspaceService& workspace,
                                                       uimodel::PresentationTextCatalog textCatalog,
                                                       TuiTextCatalog const& tuiTextCatalog)
    : _workspace{workspace}
    , _textCatalog{std::move(textCatalog)}
    , _tuiTextCatalog{tuiTextCatalog}
    , _filterCompleter{completion}
  {
  }

  std::optional<rt::CompletionResult> CommandCompletionProvider::completeCommand(std::string_view const draft)
  {
    return completeCommandDraft(
      _textCatalog,
      _tuiTextCatalog,
      draft,
      CommandCompletionContext{
        .builtinPresentations = rt::builtinTrackPresentationPresets(),
        .customPresentations = _workspace.customPresets(),
        .filterCompleter = [this](std::string_view const text, std::size_t const cursor, std::size_t const limit)
          -> std::optional<rt::CompletionResult> { return _filterCompleter.complete(text, cursor, limit); },
      });
  }

  std::optional<rt::CompletionResult> CommandCompletionProvider::completeFilter(std::string_view const draft)
  {
    return _filterCompleter.complete(draft, draft.size(), kInputCompletionResultLimit);
  }
} // namespace ao::tui
