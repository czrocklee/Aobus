// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/RouteStore.h"

#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include <ao/Error.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kBreakerWindow = std::size_t{5};
    constexpr auto kBreakerThreshold = std::size_t{3};

    // Pure breaker decision over one route's outcomes in file order: pause when a full
    // window of post-reset outcomes exists and it contains at least the threshold of
    // rejects. Shared by paused() and statistics() so the two views can never disagree.
    bool pausedFrom(std::vector<ReviewOutcome const*> const& rows, std::string_view resetTime)
    {
      auto window = std::vector<ReviewOutcome const*>{};

      for (auto const* row : std::views::reverse(rows))
      {
        if (window.size() == kBreakerWindow)
        {
          break;
        }

        if (row->timestamp > resetTime)
        {
          window.push_back(row);
        }
      }

      if (window.size() < kBreakerWindow)
      {
        return false;
      }

      auto const unusable = static_cast<std::size_t>(std::ranges::count_if(
        window, [](ReviewOutcome const* outcome) { return outcome->verdict == ReviewVerdict::Reject; }));
      return unusable >= kBreakerThreshold;
    }
  } // namespace

  RouteStore::RouteStore(std::filesystem::path out)
    : _out{std::move(out)}
  {
  }

  Result<> RouteStore::record(std::string_view phaseId, ReviewVerdict verdict, std::string_view reason)
  {
    auto existing = readReviewOutcomes(_out / "review-outcomes.yaml");

    if (!existing)
    {
      return std::unexpected{existing.error()};
    }

    if (std::ranges::any_of(
          existing->outcomes, [&](ReviewOutcome const& outcome) { return outcome.phaseId == phaseId; }))
    {
      return makeError(Error::Code::InvalidState, std::format("{}: review verdict is already terminal", phaseId));
    }

    auto route = loadManifestRoute(_out / std::string{phaseId} / "manifest.yaml");

    if (!route)
    {
      return std::unexpected{route.error()};
    }

    auto document = std::format("schema: aobus-fleet-review-outcome/v1\n"
                                "event: review-recorded\n"
                                "phase-id: {}\n"
                                "route-key: {}\n"
                                "verdict: {}\n"
                                "reason: {}\n"
                                "timestamp: {}\n",
                                yamlScalar(phaseId),
                                yamlScalar(*route),
                                toString(verdict),
                                yamlScalar(reason),
                                yamlScalar(utcTimestamp()));
    return appendYamlDocument(_out / "review-outcomes.yaml", document);
  }

  Result<std::map<std::string, std::string, std::less<>>> RouteStore::latestResets() const
  {
    auto resets = readScalarStream(_out / "route-resets.yaml", "aobus-fleet-route-reset/v1");

    if (!resets)
    {
      return std::unexpected{resets.error()};
    }

    auto result = std::map<std::string, std::string, std::less<>>{};

    for (auto const& document : resets->documents)
    {
      auto routeIt = document.find("route-key");

      if (auto timeIt = document.find("timestamp"); routeIt != document.end() && timeIt != document.end())
      {
        if (auto& latest = result[routeIt->second]; timeIt->second > latest)
        {
          latest = timeIt->second;
        }
      }
    }

    return result;
  }

  Result<bool> RouteStore::paused(std::string_view route) const
  {
    auto outcomes = readReviewOutcomes(_out / "review-outcomes.yaml");

    if (!outcomes)
    {
      return std::unexpected{outcomes.error()};
    }

    auto resets = latestResets();

    if (!resets)
    {
      return std::unexpected{resets.error()};
    }

    auto rows = std::vector<ReviewOutcome const*>{};

    for (auto const& outcome : outcomes->outcomes)
    {
      if (outcome.route == route)
      {
        rows.push_back(&outcome);
      }
    }

    auto const resetIt = resets->find(route);
    return pausedFrom(rows, resetIt == resets->end() ? std::string_view{} : resetIt->second);
  }

  Result<std::vector<RouteStatistics>> RouteStore::statistics(std::size_t window, bool* trailingCorruption) const
  {
    // Both stream files are read exactly once; every per-route view below is derived
    // from these two in-memory snapshots.
    auto outcomes = readReviewOutcomes(_out / "review-outcomes.yaml");

    if (!outcomes)
    {
      return std::unexpected{outcomes.error()};
    }

    auto resets = latestResets();

    if (!resets)
    {
      return std::unexpected{resets.error()};
    }

    if (trailingCorruption != nullptr)
    {
      *trailingCorruption = outcomes->trailingCorruption;
    }

    auto grouped = std::map<std::string, std::vector<ReviewOutcome const*>, std::less<>>{};

    for (auto const& outcome : outcomes->outcomes)
    {
      grouped[outcome.route].push_back(&outcome);
    }

    auto result = std::vector<RouteStatistics>{};

    for (auto& [route, rows] : grouped)
    {
      auto const resetIt = resets->find(route);
      auto const resetTime = resetIt == resets->end() ? std::string_view{} : std::string_view{resetIt->second};
      auto statistics =
        RouteStatistics{.route = route, .usable = 0, .unusable = 0, .paused = pausedFrom(rows, resetTime)};

      std::erase_if(rows, [&](ReviewOutcome const* row) { return row->timestamp <= resetTime; });

      if (rows.size() > window)
      {
        rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(window));
      }

      for (auto const* row : rows)
      {
        if (row->verdict == ReviewVerdict::Reject)
        {
          ++statistics.unusable;
        }
        else
        {
          ++statistics.usable;
        }
      }

      result.push_back(std::move(statistics));
    }

    return result;
  }

  Result<> RouteStore::reset(std::string_view route)
  {
    auto const timestamp = utcTimestamp();
    auto document = std::format("schema: aobus-fleet-route-reset/v1\n"
                                "event: route-reset\n"
                                "route-key: {}\n"
                                "timestamp: {}\n",
                                yamlScalar(route),
                                yamlScalar(timestamp));

    if (auto reset = appendYamlDocument(_out / "route-resets.yaml", document); !reset)
    {
      return reset;
    }

    auto audit = std::format("schema: aobus-fleet-audit-event/v1\n"
                             "event: route-reset\n"
                             "route-key: {}\n"
                             "timestamp: {}\n",
                             yamlScalar(route),
                             yamlScalar(timestamp));
    return appendYamlDocument(_out / "audit.yaml", audit);
  }
} // namespace ao::fleet
