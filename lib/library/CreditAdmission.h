// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/Credits.h>

#include <cstddef>

namespace ao::library::detail
{
  /** Trims the borrowed name and role boundaries without admitting the entry. */
  CreditView trimCredit(CreditView entry);

  /**
   * Trims one entry and admits its closed kind and nonblank name. Scalar text
   * admission stays with the caller, which validates or normalizes the result.
   */
  Result<CreditView> admitCreditShape(CreditView entry, std::size_t index);

  /** Visits entries stably grouped by kind, in `CreditKind` order. */
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
