// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace ao::rt
{
  enum class CompletionDetailKind : std::uint8_t
  {
    None,
    ResolvedText,
    Field,
    Alias,
    Operator,
    LogicalOperator,
    Frequency,
  };

  struct CompletionDetail final
  {
    CompletionDetailKind kind = CompletionDetailKind::None;
    std::string resolvedText{};
    std::uint32_t frequency = 0;

    static CompletionDetail makeResolvedText(std::string text)
    {
      return CompletionDetail{.kind = CompletionDetailKind::ResolvedText, .resolvedText = std::move(text)};
    }

    static CompletionDetail makeUsageFrequency(std::uint32_t count)
    {
      return CompletionDetail{.kind = CompletionDetailKind::Frequency, .frequency = count};
    }

    bool operator==(CompletionDetail const&) const = default;
  };

  struct CompletionItem final
  {
    std::string displayText;
    std::string insertText;
    CompletionDetail detail{};
    std::uint32_t rank = 0;
  };
} // namespace ao::rt
