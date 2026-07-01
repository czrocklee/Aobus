// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::tui
{
  class CommandCompletionState final
  {
  public:
    std::optional<rt::CompletionResult> const& result() const noexcept { return _optResult; }
    std::int32_t selection() const noexcept { return _selection; }

    void set(std::optional<rt::CompletionResult> optResult);
    bool moveSelection(std::int32_t delta);
    bool applyTo(std::string& draft);
    void clear();

  private:
    std::optional<rt::CompletionResult> _optResult{};
    std::int32_t _selection = 0;
  };
} // namespace ao::tui
