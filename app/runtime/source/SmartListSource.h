// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/source/IndexedTrackSequence.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::query
{
  struct ExecutionPlan;
}

namespace ao::rt
{
  class SmartListEvaluator;

  /** Reactive predicate source owned by the runtime source pipeline. */
  class SmartListSource final : public TrackSource
  {
  public:
    SmartListSource(TrackSourceLease sourceLease, SmartListEvaluator& evaluator);
    ~SmartListSource() override;

    SmartListSource(SmartListSource const&) = delete;
    SmartListSource& operator=(SmartListSource const&) = delete;
    SmartListSource(SmartListSource&&) = delete;
    SmartListSource& operator=(SmartListSource&&) = delete;

    void setExpression(std::string expr);
    void reload();

    std::size_t size() const override { return _members.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _members.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    bool hasError() const { return _current.optError.has_value(); }
    std::optional<Error> const& error() const { return _current.optError; }
    std::string const& expression() const { return _current.expression; }
    TrackSource const& source() const { return _sourceLease.source(); }

  private:
    friend class SmartListEvaluator;

    struct QueryState final
    {
      std::string expression;
      std::unique_ptr<query::ExecutionPlan> planPtr;
      std::optional<Error> optError;
    };

    void applyPendingState();
    void invalidateFromEvaluator() noexcept { invalidate(); }
    void replaceMembers(std::vector<TrackId> members);
    void discardSnapshot() noexcept override;

    TrackSourceLease _sourceLease;
    SmartListEvaluator* _evaluator = nullptr;

    IndexedTrackSequence _members;
    QueryState _current;
    query::PlanEvaluator _planEvaluator;

    std::optional<QueryState> _optPending;
  };
} // namespace ao::rt
