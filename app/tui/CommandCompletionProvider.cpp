// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletionProvider.h"

#include "CommandCompletion.h"
#include "LibraryController.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::tui
{
  CommandCompletionProvider::CommandCompletionProvider(LibraryController& library,
                                                       rt::CompletionService& completion,
                                                       rt::WorkspaceService& workspace)
    : _library{library}, _completion{completion}, _workspace{workspace}, _expressionCompleter{completion}
  {
  }

  std::optional<rt::CompletionResult> CommandCompletionProvider::complete(std::string_view const draft)
  {
    return completeCommandDraft(
      draft,
      CommandCompletionContext{
        .lists = _library.libraryEntries(),
        .artists = _completion.valuesFor(rt::TrackField::Artist),
        .builtinPresentations = rt::builtinTrackPresentationPresets(),
        .customPresentations = _workspace.customPresets(),
        .expressionCompleter = [this](std::string_view const text, std::size_t const cursor, std::size_t const limit)
          -> std::optional<rt::CompletionResult> { return _expressionCompleter.complete(text, cursor, limit); },
      });
  }
} // namespace ao::tui
