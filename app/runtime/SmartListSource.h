// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSource.h"

#include <ao/library/MusicLibrary.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/PlanEvaluator.h>

#include <flat_set>
#include <memory>
#include <string>

namespace ao::app
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
    SmartListSource(TrackSource& source, ao::library::MusicLibrary& ml, SmartListEvaluator& evaluator);
    ~SmartListSource() override;

    void setExpression(std::string expr);
    void reload();

    // TrackSource interface
    std::size_t size() const override { return _members.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_members.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    using TrackSource::notifyUpdated;
    void notifyUpdated(TrackId id) override;

    bool hasError() const { return _hasError; }
    std::string const& errorMessage() const { return _errorMessage; }
    TrackSource& source() const { return _source; }

  private:
    friend class SmartListEvaluator;

    void stageExpression(std::string expr);
    void applyStagedState();

    TrackSource& _source;
    ao::library::MusicLibrary& _ml;
    SmartListEvaluator* _evaluator = nullptr;

    std::flat_set<TrackId> _members;
    std::string _expression;
    bool _hasError = false;
    std::string _errorMessage;

    std::unique_ptr<ao::query::ExecutionPlan> _plan;
    ao::query::PlanEvaluator _planEvaluator;

    // Staging for lazy/batch updates
    std::string _stagedExpression;
    std::unique_ptr<ao::query::ExecutionPlan> _stagedPlan;
    bool _stagedHasError = false;
    std::string _stagedErrorMessage;
    bool _dirty = true;
  };
}
