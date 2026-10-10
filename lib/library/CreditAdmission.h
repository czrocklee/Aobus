// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/Credits.h>

#include <cstddef>
#include <string_view>

namespace ao::library::detail
{
  /** One credit after ASCII boundary trimming; it borrows the caller's text. */
  struct TrimmedCredit final
  {
    std::string_view name{};
    CreditKind kind = CreditKind::Performer;
    std::string_view role{};
  };

  /** Trims the name and role boundaries without admitting the entry. */
  TrimmedCredit trimCredit(CreditView entry);

  /**
   * Trims one entry and admits its closed kind and nonblank name. Scalar text
   * admission stays with the caller, which validates or normalizes the result.
   */
  Result<TrimmedCredit> admitCreditShape(CreditView entry, std::size_t index);

  /** Visits entries stably grouped into the four fixed kind segments. */
  template<typename Entries, typename Visit>
  void forEachCreditInKindOrder(Entries& entries, Visit const& visit)
  {
    for (std::size_t kindIndex = 0; kindIndex < kCreditKindCount; ++kindIndex)
    {
      for (auto&& entry : entries)
      {
        if (entry.kind == static_cast<CreditKind>(kindIndex))
        {
          visit(entry);
        }
      }
    }
  }
} // namespace ao::library::detail
