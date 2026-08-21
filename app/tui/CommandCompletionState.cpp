// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandCompletionState.h"

#include "SelectionNavigation.h"
#include <ao/rt/completion/CompletionResult.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ao::tui
{
  namespace
  {
    std::int32_t wrapSelection(std::int32_t const current, std::int32_t const delta, std::int32_t const itemCount)
    {
      auto next = (static_cast<std::int64_t>(current) + static_cast<std::int64_t>(delta)) % itemCount;

      if (next < 0)
      {
        next += itemCount;
      }

      return static_cast<std::int32_t>(next);
    }
  } // namespace

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
    _selection = wrapSelection(_selection, delta, count);
    return true;
  }

  bool CommandCompletionState::moveSelectionByPage(std::int32_t const delta)
  {
    if (!_optResult || _optResult->items.empty())
    {
      return false;
    }

    _selection = ao::tui::moveSelection(_selection, delta, _optResult->items.size());
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

    if (result.replaceBegin > result.replaceEnd || result.replaceEnd > draft.size())
    {
      clear();
      return false;
    }

    draft.replace(result.replaceBegin, result.replaceEnd - result.replaceBegin, item.insertText);
    clear();
    return true;
  }

  void CommandCompletionState::clear()
  {
    _optResult.reset();
    _selection = 0;
  }
} // namespace ao::tui
