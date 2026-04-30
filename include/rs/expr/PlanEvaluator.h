// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/expr/ExecutionPlan.h>
#include <rs/library/TrackView.h>

#include <string_view>
#include <vector>

namespace rs::expr
{

  /**
   * PlanEvaluator - Fast execution engine for compiled queries.
   *
   * Uses bloom filter for fast tag rejection and linear instruction execution.
   */
  class PlanEvaluator
  {
  public:
    PlanEvaluator() = default;

    /**
     * Check if a track matches the execution plan using bloom filter fast-path.
     *
     * @param plan The compiled execution plan
     * @param track The track view to evaluate
     * @return true if the track matches the query
     */
    bool matches(ExecutionPlan const& plan, rs::library::TrackView const& track) const;

    /**
     * Evaluate the full execution plan (without bloom filter optimization).
     *
     * @param plan The compiled execution plan
     * @param track The track view to evaluate
     * @return true if the track matches the query
     */
    bool evaluateFull(ExecutionPlan const& plan, rs::library::TrackView const& track) const;

  private:
    // Register stack for evaluation
    mutable std::vector<std::int64_t> _registers;
  };

} // namespace rs::expr
