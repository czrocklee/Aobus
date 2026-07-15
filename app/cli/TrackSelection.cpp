// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackSelection.h"

#include "CommandError.h"
#include "QueryHelp.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/library/LibraryReader.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::cli
{
  std::vector<TrackId> queryMatchingTrackIds(library::MusicLibrary& library, std::string const& filter)
  {
    auto const transaction = library.readTransaction();
    auto const reader = library.tracks().reader(transaction);
    auto ids = std::vector<TrackId>{};

    if (filter.empty())
    {
      for (auto const& [id, view] : reader)
      {
        std::ignore = view;
        ids.push_back(id);
      }

      return ids;
    }

    auto const expr = query::parse(filter);

    if (!expr)
    {
      auto const& error = expr.error();
      throwCommandError(error, "filter error: {}{}", error.message, queryFilterUsageHint());
    }

    auto const plan = query::compileQuery(*expr);

    if (!plan)
    {
      auto const& error = plan.error();
      throwCommandError(error, "filter error: {}{}", error.message, queryFilterUsageHint());
    }

    auto evaluator = query::PlanEvaluator{};
    auto dictionaryCache = library::DictionaryReadCache{library.dictionary()};
    auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
    auto binding = query::PlanBinding{*plan, dictionaryContext};

    for (auto const& [id, view] : reader)
    {
      if (evaluator.matches(binding, view))
      {
        ids.push_back(id);
      }
    }

    return ids;
  }

  std::vector<TrackId> requireTrackIds(rt::LibraryReader& reader, std::vector<std::uint32_t> const& rawIds)
  {
    auto ids = std::vector<TrackId>{};
    ids.reserve(rawIds.size());

    for (auto const rawId : rawIds)
    {
      auto const id = TrackId{rawId};

      if (!reader.trackRow(id))
      {
        throwCommandError(Error::Code::NotFound, "track not found: {}", id);
      }

      if (!std::ranges::contains(ids, id))
      {
        ids.push_back(id);
      }
    }

    return ids;
  }
} // namespace ao::cli
