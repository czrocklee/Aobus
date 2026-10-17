// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/Credits.h>

#include "CreditAdmission.h"
#include "TextAdmission.h"
#include <ao/Error.h>

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
        auto shapeRes =
          detail::admitCreditShape(CreditView{.name = entry.name, .kind = entry.kind, .role = entry.role}, index);

        if (!shapeRes)
        {
          return std::unexpected{shapeRes.error()};
        }

        auto nameRes = detail::normalizeLibraryText(shapeRes->name, std::format("Credit entry {} name", index));

        if (!nameRes)
        {
          return std::unexpected{nameRes.error()};
        }

        auto roleRes = detail::normalizeLibraryText(shapeRes->role, std::format("Credit entry {} role", index));

        if (!roleRes)
        {
          return std::unexpected{roleRes.error()};
        }

        credits.push_back({.name = std::move(*nameRes), .kind = shapeRes->kind, .role = std::move(*roleRes)});
      }

      // Within-kind order and duplicate multiplicity are retained.
      auto canonical = std::vector<Credit>{};
      canonical.reserve(credits.size());
      detail::forEachCreditInKindOrder(
        credits, [&canonical](Credit& credit) { canonical.push_back(std::move(credit)); });
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
      case CreditKind::Lyricist: return "lyricist";
      case CreditKind::Arranger: return "arranger";
      case CreditKind::Writer: return "writer";
      case CreditKind::Producer: return "producer";
      case CreditKind::Engineer: return "engineer";
      case CreditKind::Mixer: return "mixer";
      case CreditKind::DjMixer: return "dj-mixer";
      case CreditKind::Remixer: return "remixer";
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
