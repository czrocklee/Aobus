// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Model.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/CompletionService.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace ao::tui
{
  struct CommandCompletionContext final
  {
    std::span<LibraryNavItem const> lists{};
    std::span<rt::VocabularyEntry const> artists{};
    std::span<rt::TrackPresentationPreset const> builtinPresentations{};
    std::span<rt::CustomTrackPresentationPreset const> customPresentations{};
    std::function<std::optional<rt::CompletionResult>(std::string_view text, std::size_t cursor, std::size_t limit)>
      expressionCompleter{};
  };

  std::optional<rt::CompletionResult> completeCommandDraft(std::string_view draft,
                                                           CommandCompletionContext const& context,
                                                           std::size_t limit = 8);
} // namespace ao::tui
