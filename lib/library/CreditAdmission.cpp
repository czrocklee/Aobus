// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CreditAdmission.h"

#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/utility/String.h>

#include <cstddef>
#include <format>

namespace ao::library::detail
{
  CreditView trimCredit(CreditView const entry)
  {
    return {.name = utility::trim(entry.name), .kind = entry.kind, .role = utility::trim(entry.role)};
  }

  Result<CreditView> admitCreditShape(CreditView const entry, std::size_t const index)
  {
    auto const trimmed = trimCredit(entry);

    if (!isValidCreditKind(trimmed.kind) || trimmed.name.empty())
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Credit entry {} has an invalid kind or blank name", index));
    }

    return trimmed;
  }
} // namespace ao::library::detail
