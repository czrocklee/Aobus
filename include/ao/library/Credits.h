// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  enum class CreditKind : std::uint8_t
  {
    Conductor,
    Ensemble,
    Soloist,
    Performer,
  };

  constexpr std::size_t kCreditKindCount = 4;

  struct Credit final
  {
    std::string name{};
    CreditKind kind = CreditKind::Performer;
    std::string role{};

    bool operator==(Credit const&) const = default;
  };

  struct CreditView final
  {
    std::string_view name{};
    CreditKind kind = CreditKind::Performer;
    std::string_view role{};
  };

  constexpr bool isValidCreditKind(CreditKind kind) noexcept
  {
    return static_cast<std::size_t>(kind) < kCreditKindCount;
  }

  std::string_view creditKindToken(CreditKind kind) noexcept;
  std::optional<CreditKind> parseCreditKind(std::string_view token) noexcept;

  /** Trim ASCII boundaries, admit scalar UTF-8/NFC, and stably group by kind.
   * Blank names and invalid kinds reject the list. Duplicates are preserved.
   */
  Result<std::vector<Credit>> normalizeCredits(std::span<CreditView const> entries);
  Result<std::vector<Credit>> normalizeCredits(std::span<Credit const> entries);
} // namespace ao::library
