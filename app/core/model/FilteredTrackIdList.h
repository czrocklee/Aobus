// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/model/TrackIdList.h"

#include <rs/core/MusicLibrary.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/PlanEvaluator.h>

#include <flat_set>
#include <memory>
#include <string>

namespace app::core::model
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
    FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& ml, SmartListEngine& engine);
    ~FilteredTrackIdList() override;

    void setExpression(std::string expr);
    void reload();

    // TrackIdList interface
    std::size_t size() const override { return _members.size(); }
    TrackId trackIdAt(std::size_t index) const override { return *(_members.begin() + index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    void notifyTrackDataChanged(TrackId id) override;

    bool hasError() const { return _hasError; }
    std::string const& errorMessage() const { return _errorMessage; }

  private:
    friend class SmartListEngine;

    void stageExpression(std::string expr);
    void applyStagedState();

    TrackIdList* _source = nullptr;
    rs::core::MusicLibrary* _ml = nullptr;
    SmartListEngine* _engine = nullptr;

    std::flat_set<TrackId> _members;
    std::string _expression;
    bool _hasError = false;
    std::string _errorMessage;

    std::unique_ptr<rs::expr::ExecutionPlan> _plan;
    rs::expr::PlanEvaluator _evaluator;

    // Staging for lazy/batch updates
    std::string _stagedExpression;
    std::unique_ptr<rs::expr::ExecutionPlan> _stagedPlan;
    bool _stagedHasError = false;
    std::string _stagedErrorMessage;
    bool _dirty = true;
  };

} // namespace app::core::model
