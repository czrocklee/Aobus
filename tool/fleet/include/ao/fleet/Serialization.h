// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/fleet/Model.h>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::fleet
{
  struct ReviewOutcome
  {
    std::string phaseId;
    std::string route;
    ReviewVerdict verdict = ReviewVerdict::Reject;
    std::string reason;
    std::string timestamp;
  };

  struct StreamReadResult
  {
    std::vector<ReviewOutcome> outcomes;
    bool trailingCorruption = false;
  };

  struct ScalarStreamResult
  {
    std::vector<std::map<std::string, std::string, std::less<>>> documents;
    bool trailingCorruption = false;
  };

  Result<Registry> loadRegistry(std::filesystem::path const& path);
  Result<PhaseIntent> loadIntent(std::filesystem::path const& path);
  Result<std::vector<PhaseIntent>> loadIntents(std::vector<std::filesystem::path> const& paths);
  Result<ResolvedPhase> resolvePhase(Registry const& registry, PhaseIntent const& intent);

  std::string emitIntent(PhaseIntent const& intent);
  std::string emitResolved(ResolvedPhase const& phase);
  std::string emitManifest(ReviewManifest const& manifest);
  std::string emitEvidence(ReviewManifest const& manifest);
  std::string emitTraceEvent(std::string_view event, std::map<std::string, std::string, std::less<>> const& fields);

  Result<> appendYamlDocument(std::filesystem::path const& path, std::string_view document);
  Result<StreamReadResult> readReviewOutcomes(std::filesystem::path const& path);
  Result<ScalarStreamResult> readScalarStream(std::filesystem::path const& path, std::string_view schema);
  Result<std::string> loadManifestRoute(std::filesystem::path const& path);

  std::string utcTimestamp();
  std::string yamlScalar(std::string_view value);
  std::optional<ReviewVerdict> parseReviewVerdict(std::string_view value);
} // namespace ao::fleet
