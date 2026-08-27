// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/completion/CompletionResult.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tui
{
  class ShellInteractionModel;
  inline constexpr std::size_t kInputCompletionResultLimit = 8;

  struct CommandCompletionContext final
  {
    std::span<rt::TrackPresentationPreset const> builtinPresentations{};
    std::span<rt::CustomTrackPresentationPreset const> customPresentations{};
    std::function<std::optional<rt::CompletionResult>(std::string_view text, std::size_t cursor, std::size_t limit)>
      filterCompleter{};
  };

  std::optional<rt::CompletionResult> completeCommandDraft(i18n::MessageCatalog const& textCatalog,
                                                           std::string_view draft,
                                                           CommandCompletionContext const& context,
                                                           std::size_t limit = kInputCompletionResultLimit);
  std::string commandCompletionSuffix(ShellInteractionModel const& shell);
} // namespace ao::tui
