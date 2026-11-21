// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/library/ScanPlan.h>

#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace ao::rt
{
  // Old URI -> sole eligible destination. An unresolved endpoint may not become a New import or a Missing source.
  using ScanSpellingFallbackPairs = std::unordered_map<std::string, std::string>;

  // Only globally unique audio identities can move; reserved endpoints additionally have to match each other.
  // Returns the Missing sources removed from the plan, for ordinary moves refused later by CUE binding.
  std::vector<ScanItem> classifyMovedEntries(std::vector<ScanItem>& items,
                                             ScanSpellingFallbackPairs const& fallbackPairs,
                                             std::stop_token stopToken);
} // namespace ao::rt
