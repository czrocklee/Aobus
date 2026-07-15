// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/TrackView.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ao::library
{
  class DictionaryReadContext;
}

namespace ao::query
{
  struct ExecutionPlan;

  /**
   * Resolves one immutable execution plan against one bounded dictionary context.
   *
   * The binding borrows the plan and, when supplied, the dictionary context; each
   * borrowed object must outlive the binding. Construct it once per evaluation
   * batch. Numeric IDs and the tag bloom mask are derived accelerators, while
   * symbol text in the plan remains the semantic authority.
   */
  class PlanBinding final
  {
  public:
    /// @pre @p plan outlives the binding and is context-free (`requiresDictionary == false`).
    explicit PlanBinding(ExecutionPlan const& plan);

    /// @pre @p plan and @p dictionary both outlive the binding.
    PlanBinding(ExecutionPlan const& plan, library::DictionaryReadContext& dictionary);
    ~PlanBinding();

    PlanBinding(PlanBinding const&) = delete;
    PlanBinding& operator=(PlanBinding const&) = delete;
    PlanBinding(PlanBinding&&) noexcept;
    PlanBinding& operator=(PlanBinding&&) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class PlanEvaluator;
  };

  /**
   * PlanEvaluator - Fast execution engine for compiled queries.
   *
   * Uses bloom filter for fast tag rejection and linear instruction execution.
   */
  class PlanEvaluator
  {
  public:
    PlanEvaluator() = default;

    bool matches(PlanBinding const& binding, library::TrackView const& track) const;

    /**
     * Convenience evaluation for a context-free plan.
     *
     * Requires `plan.requiresDictionary == false`. This constructs a binding per
     * call; reuse PlanBinding when evaluating a batch.
     */
    bool matches(ExecutionPlan const& plan, library::TrackView const& track) const;

    bool evaluateFull(PlanBinding const& binding, library::TrackView const& track) const;

    /// @pre `plan.requiresDictionary == false`.
    bool evaluateFull(ExecutionPlan const& plan, library::TrackView const& track) const;

  private:
    // Register stack for evaluation
    mutable std::vector<std::int64_t> _registers;
  };
} // namespace ao::query
