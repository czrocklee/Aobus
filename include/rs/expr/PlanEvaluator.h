/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <rs/core/TrackLayout.h>
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
     *
     * @param plan The compiled execution plan
     * @param track The track view to evaluate
     * @return true if the track matches the query
     */
    bool matches(ExecutionPlan const& plan, core::TrackView const& track) const;

    /**
     * Evaluate the full execution plan (without bloom filter optimization).
     *
     * @param plan The compiled execution plan
     * @param track The track view to evaluate
     * @return true if the track matches the query
     */
    bool evaluateFull(ExecutionPlan const& plan, core::TrackView const& track) const;

  private:
    std::int64_t loadField(core::TrackView const& track, Field field) const;
    std::string_view loadStringField(core::TrackView const& track, Field field) const;
    std::int64_t loadConstant(Instruction const& instr) const;

    // Pointer to current plan for string constant access
    mutable ExecutionPlan const* _currentPlan = nullptr;

    // Register stack for evaluation
    mutable std::vector<std::int64_t> _registers;
  };

} // namespace rs::expr
