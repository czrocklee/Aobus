// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletionState.h"

#include <ao/rt/completion/CompletionResult.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ao::tui
{
  void CommandCompletionState::set(std::optional<rt::CompletionResult> optResult)
  {
    if (!optResult || optResult->items.empty())
    {
      clear();
      return;
    }

    _optResult = std::move(optResult);
    _selection = 0;
  }

  bool CommandCompletionState::moveSelection(std::int32_t const delta)
  {
    if (!_optResult || _optResult->items.empty())
    {
      return false;
    }

    auto const count = static_cast<std::int32_t>(_optResult->items.size());
    _selection = std::clamp(_selection + delta, 0, count - 1);
    return true;
  }

  bool CommandCompletionState::applyTo(std::string& draft)
  {
    if (!_optResult || _optResult->items.empty())
    {
      return false;
    }

    auto const index = static_cast<std::size_t>(std::max<std::int32_t>(0, _selection)) % _optResult->items.size();
    auto const& result = *_optResult;
    auto const& item = result.items[index];
    auto const replaceBegin = std::min(result.replaceBegin, draft.size());
    auto const replaceEnd = std::min(result.replaceEnd, draft.size());

    draft.replace(replaceBegin, replaceEnd - replaceBegin, item.insertText);
    clear();
    return true;
  }

  void CommandCompletionState::clear()
  {
    _optResult.reset();
    _selection = 0;
  }
} // namespace ao::tui
