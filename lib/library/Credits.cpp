// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/Credits.h>

#include "TextAdmission.h"
#include <ao/Error.h>
#include <ao/utility/String.h>

#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  namespace
  {
    template<typename Entry>
    Result<std::vector<Credit>> normalizeEntries(std::span<Entry const> entries)
    {
      auto credits = std::vector<Credit>{};
      credits.reserve(entries.size());

      for (std::size_t index = 0; index < entries.size(); ++index)
      {
        auto const& entry = entries[index];
        auto const name = utility::trim(entry.name);
        auto const role = utility::trim(entry.role);

        if (!isValidCreditKind(entry.kind) || name.empty())
        {
          return makeError(
            Error::Code::InvalidInput, std::format("Credit entry {} has an invalid kind or blank name", index));
        }

        auto nameRes = detail::normalizeLibraryText(name, std::format("Credit entry {} name", index));

        if (!nameRes)
        {
          return std::unexpected{nameRes.error()};
        }

        auto roleRes = detail::normalizeLibraryText(role, std::format("Credit entry {} role", index));

        if (!roleRes)
        {
          return std::unexpected{roleRes.error()};
        }

        credits.push_back({.name = std::move(*nameRes), .kind = entry.kind, .role = std::move(*roleRes)});
      }

      // Four fixed buckets; within-kind order and duplicate multiplicity are retained.
      auto canonical = std::vector<Credit>{};
      canonical.reserve(credits.size());

      for (std::size_t index = 0; index < kCreditKindCount; ++index)
      {
        for (auto& credit : credits)
        {
          if (credit.kind == static_cast<CreditKind>(index))
          {
            canonical.push_back(std::move(credit));
          }
        }
      }

      return canonical;
    }
  } // namespace

  std::string_view creditKindToken(CreditKind kind) noexcept
  {
    switch (kind)
    {
      case CreditKind::Conductor: return "conductor";
      case CreditKind::Ensemble: return "ensemble";
      case CreditKind::Soloist: return "soloist";
      case CreditKind::Performer: return "performer";
    }

    return {};
  }

  std::optional<CreditKind> parseCreditKind(std::string_view token) noexcept
  {
    for (std::size_t index = 0; index < kCreditKindCount; ++index)
    {
      if (auto const kind = static_cast<CreditKind>(index); creditKindToken(kind) == token)
      {
        return kind;
      }
    }

    return std::nullopt;
  }

  Result<std::vector<Credit>> normalizeCredits(std::span<CreditView const> entries)
  {
    return normalizeEntries(entries);
  }

  Result<std::vector<Credit>> normalizeCredits(std::span<Credit const> entries)
  {
    return normalizeEntries(entries);
  }
} // namespace ao::library
