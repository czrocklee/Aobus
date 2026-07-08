// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "council/CouncilSchema.h"
#include <ao/Error.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ao::council
{
  struct ScalarStreamResult
  {
    std::vector<std::map<std::string, std::string, std::less<>>> documents = {};
    bool trailingCorruption = false;
  };

  Result<Registry> loadRegistry(std::filesystem::path const& path);
  Result<PhaseIntent> loadIntent(std::filesystem::path const& path);
  Result<std::vector<PhaseIntent>> loadIntents(std::vector<std::filesystem::path> const& paths);
  Result<ResolvedPhase> resolvePhase(Registry const& registry, PhaseIntent const& intent);

  std::string emitIntent(PhaseIntent const& intent);
  std::string emitResolved(ResolvedPhase const& phase);
  std::string emitManifest(ReviewManifest const& manifest);
  std::string emitTraceEvent(std::string_view event, std::map<std::string, std::string, std::less<>> const& fields);

  Result<> appendYamlDocument(std::filesystem::path const& path, std::string_view document);
  Result<ScalarStreamResult> readScalarStream(std::filesystem::path const& path, std::string_view schema);

  std::string utcTimestamp();
  std::string yamlScalar(std::string_view value);
} // namespace ao::council
