// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "IndexedTrackSequence.h"
#include "TrackSource.h"
#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/PlanEvaluator.h>

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

  /**
   * SmartListSource - A reactive smart list that filters another TrackSource.
   *
   * It holds its own members and relies on SmartListEvaluator to drive updates
   * based on library changes.
   */
  class SmartListSource final : public TrackSource
  {
  public:
    SmartListSource(TrackSourceLease sourceLease, SmartListEvaluator& evaluator);
    ~SmartListSource() override;

    using TrackSource::notifyUpdated;

    SmartListSource(SmartListSource const&) = delete;
    SmartListSource& operator=(SmartListSource const&) = delete;
    SmartListSource(SmartListSource&&) = delete;
    SmartListSource& operator=(SmartListSource&&) = delete;

    void setExpression(std::string expr);
    void reload();

    // TrackSource interface
    std::size_t size() const override { return _members.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _members.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    void notifyUpdated(TrackId id) override;

    bool hasError() const { return _current.optError.has_value(); }
    std::optional<Error> const& error() const { return _current.optError; }
    std::string const& expression() const { return _current.expression; }
    TrackSource& source() const { return _sourceLease.source(); }

  private:
    friend class SmartListEvaluator;

    struct QueryState final
    {
      std::string expression;
      std::unique_ptr<query::ExecutionPlan> planPtr;
      std::optional<Error> optError;
    };

    void applyPendingState();
    void replaceMembers(std::vector<TrackId> members);

    TrackSourceLease _sourceLease;
    SmartListEvaluator* _evaluator = nullptr;

    IndexedTrackSequence _members;
    QueryState _current;
    query::PlanEvaluator _planEvaluator;

    std::optional<QueryState> _optPending;
  };
} // namespace ao::rt
