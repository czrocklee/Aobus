// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/model/TrackIdList.h>

#include <ao/library/MusicLibrary.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/PlanEvaluator.h>

#include <flat_set>
#include <memory>
#include <string>

namespace ao::model
{
  class SmartListEngine;

  /**
   * FilteredTrackIdList - A reactive smart list that filters another TrackIdList.
   *
   * It holds its own members and relies on SmartListEngine to drive updates
   * based on library changes.
   */
  class FilteredTrackIdList final : public TrackIdList
  {
  public:
    FilteredTrackIdList(TrackIdList& source, ao::library::MusicLibrary& ml, SmartListEngine& engine);
    ~FilteredTrackIdList() override;

    void setExpression(std::string expr);
    void reload();

    // TrackIdList interface
    std::size_t size() const override { return _members.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_members.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    using TrackIdList::notifyUpdated;
    void notifyUpdated(TrackId id) override;

    bool hasError() const { return _hasError; }
    std::string const& errorMessage() const { return _errorMessage; }

  private:
    friend class SmartListEngine;

    void stageExpression(std::string expr);
    void applyStagedState();

    TrackIdList& _source;
    ao::library::MusicLibrary& _ml;
    SmartListEngine* _engine = nullptr;

    std::flat_set<TrackId> _members;
    std::string _expression;
    bool _hasError = false;
    std::string _errorMessage;

    std::unique_ptr<ao::query::ExecutionPlan> _plan;
    ao::query::PlanEvaluator _evaluator;

    // Staging for lazy/batch updates
    std::string _stagedExpression;
    std::unique_ptr<ao::query::ExecutionPlan> _stagedPlan;
    bool _stagedHasError = false;
    std::string _stagedErrorMessage;
    bool _dirty = true;
  };
} // namespace ao::model
