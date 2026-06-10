// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/RouteStore.h>
#include <ao/fleet/Serialization.h>

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

  Result<std::string> RouteStore::latestReset(std::string_view route) const
  {
    auto resets = readScalarStream(_out / "route-resets.yaml", "aobus-fleet-route-reset/v1");

    if (!resets)
    {
      return std::unexpected{resets.error()};
    }

    auto latest = std::string{};

    for (auto const& document : resets->documents)
    {
      auto routeIt = document.find("route-key");

      if (auto timeIt = document.find("timestamp");
          routeIt != document.end() && timeIt != document.end() && routeIt->second == route && timeIt->second > latest)
      {
        latest = timeIt->second;
      }
    }

    return latest;
  }

  Result<bool> RouteStore::paused(std::string_view route) const
  {
    auto outcomes = readReviewOutcomes(_out / "review-outcomes.yaml");

    if (!outcomes)
    {
      return std::unexpected{outcomes.error()};
    }

    auto resetTime = latestReset(route);

    if (!resetTime)
    {
      return std::unexpected{resetTime.error()};
    }

    auto recent = std::vector<ReviewOutcome>{};

    for (auto iterator = outcomes->outcomes.rbegin();
         iterator != outcomes->outcomes.rend() && recent.size() < kBreakerWindow;
         ++iterator)
    {
      if (iterator->route == route && iterator->timestamp > *resetTime)
      {
        recent.push_back(*iterator);
      }
    }

    if (recent.size() < kBreakerWindow)
    {
      return false;
    }

    auto const unusable = static_cast<std::size_t>(std::ranges::count_if(
      recent, [](ReviewOutcome const& outcome) { return outcome.verdict == ReviewVerdict::Reject; }));
    return unusable >= kBreakerThreshold;
  }

  Result<std::vector<RouteStatistics>> RouteStore::statistics(std::size_t window, bool* trailingCorruption) const
  {
    auto outcomes = readReviewOutcomes(_out / "review-outcomes.yaml");

    if (!outcomes)
    {
      return std::unexpected{outcomes.error()};
    }

    if (trailingCorruption != nullptr)
    {
      *trailingCorruption = outcomes->trailingCorruption;
    }

    auto grouped = std::map<std::string, std::vector<ReviewOutcome>, std::less<>>{};

    for (auto const& outcome : outcomes->outcomes)
    {
      grouped[outcome.route].push_back(outcome);
    }

    auto result = std::vector<RouteStatistics>{};

    for (auto& [route, rows] : grouped)
    {
      auto resetTime = latestReset(route);

      if (!resetTime)
      {
        return std::unexpected{resetTime.error()};
      }

      std::erase_if(rows, [&](ReviewOutcome const& row) { return row.timestamp <= *resetTime; });

      if (rows.size() > window)
      {
        rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(window));
      }

      auto statistics = RouteStatistics{.route = route};

      for (auto const& row : rows)
      {
        if (row.verdict == ReviewVerdict::Reject)
        {
          ++statistics.unusable;
        }
        else
        {
          ++statistics.usable;
        }
      }

      auto routePaused = paused(route);

      if (!routePaused)
      {
        return std::unexpected{routePaused.error()};
      }

      statistics.paused = *routePaused;
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
