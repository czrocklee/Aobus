// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/TrackView.h>
#include <rs/expr/ExecutionPlan.h>

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
     * Accepts hot view only - use for HotOnly profile.
     *
     * @param plan The compiled execution plan
     * @param hotView The hot track view to evaluate
     * @return true if the track matches the query
     */
    bool matches(ExecutionPlan const& plan, core::TrackHotView const& hotView) const;

    /**
     * Check if a track matches the execution plan using bloom filter fast-path.
     * Accepts cold view only - use for ColdOnly profile.
     *
     * @param plan The compiled execution plan
     * @param coldView The cold track view to evaluate
     * @return true if the track matches the query
     */
    bool matches(ExecutionPlan const& plan, core::TrackColdView const& coldView) const;

    /**
     * Check if a track matches the execution plan using bloom filter fast-path.
     * Accepts both hot and cold views - use for HotAndCold profile.
     *
     * @param plan The compiled execution plan
     * @param hotView The hot track view to evaluate
     * @param coldView The cold track view to evaluate
     * @return true if the track matches the query
     */
    bool matches(ExecutionPlan const& plan, core::TrackHotView const& hotView, core::TrackColdView const& coldView) const;

    /**
     * Evaluate the full execution plan (without bloom filter optimization).
     * Accepts hot view only - use for HotOnly profile.
     *
     * @param plan The compiled execution plan
     * @param hotView The hot track view to evaluate
     * @return true if the track matches the query
     */
    bool evaluateFull(ExecutionPlan const& plan, core::TrackHotView const& hotView) const;

    /**
     * Evaluate the full execution plan (without bloom filter optimization).
     * Accepts cold view only - use for ColdOnly profile.
     *
     * @param plan The compiled execution plan
     * @param coldView The cold track view to evaluate
     * @return true if the track matches the query
     */
    bool evaluateFull(ExecutionPlan const& plan, core::TrackColdView const& coldView) const;

    /**
     * Evaluate the full execution plan (without bloom filter optimization).
     * Accepts both hot and cold views - use for HotAndCold profile.
     *
     * @param plan The compiled execution plan
     * @param hotView The hot track view to evaluate
     * @param coldView The cold track view to evaluate
     * @return true if the track matches the query
     */
    bool evaluateFull(ExecutionPlan const& plan, core::TrackHotView const& hotView, core::TrackColdView const& coldView) const;

  private:
    std::int64_t loadField(core::TrackHotView const& hotView, Field field) const;
    std::string_view loadStringField(core::TrackHotView const& hotView, Field field) const;
    std::int64_t loadField(core::TrackColdView const& coldView, Field field) const;
    std::string_view loadStringField(core::TrackColdView const& coldView, Field field) const;
    std::int64_t loadConstant(Instruction const& instr) const;

    // Pointer to current plan for string constant access
    mutable ExecutionPlan const* _currentPlan = nullptr;

    // Register stack for evaluation
    mutable std::vector<std::int64_t> _registers;
  };

} // namespace rs::expr
