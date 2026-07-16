// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletionProvider.h"

#include "CommandCompletion.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::tui
{
  CommandCompletionProvider::CommandCompletionProvider(rt::CompletionService& completion,
                                                       rt::WorkspaceService& workspace)
    : _workspace{workspace}, _filterCompleter{completion}
  {
  }

  std::optional<rt::CompletionResult> CommandCompletionProvider::complete(std::string_view const draft)
  {
    return completeCommandDraft(
      draft,
      CommandCompletionContext{
        .builtinPresentations = rt::builtinTrackPresentationPresets(),
        .customPresentations = _workspace.customPresets(),
        .filterCompleter = [this](std::string_view const text, std::size_t const cursor, std::size_t const limit)
          -> std::optional<rt::CompletionResult> { return _filterCompleter.complete(text, cursor, limit); },
      });
  }
} // namespace ao::tui
