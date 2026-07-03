// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/TrackView.h>

#include <cstdint>
#include <vector>

namespace ao::query
{
  struct ExecutionPlan;

  /**
   * PlanEvaluator - Fast execution engine for compiled queries.
   *
   * Uses bloom filter for fast tag rejection and linear instruction execution.
   */
  class PlanEvaluator
  {
  public:
    PlanEvaluator() = default;

    bool matches(ExecutionPlan const& plan, library::TrackView const& track) const;

    bool evaluateFull(ExecutionPlan const& plan, library::TrackView const& track) const;

  private:
    // Register stack for evaluation
    mutable std::vector<std::int64_t> _registers;
  };
} // namespace ao::query
