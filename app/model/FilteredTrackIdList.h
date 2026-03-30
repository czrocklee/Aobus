// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackIdList.h"

#include <rs/core/DictionaryStore.h>
#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackStore.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/PlanEvaluator.h>

#include <memory>
#include <string>

namespace app::model
{

  class TrackIdList; // Forward declaration

  /**
   * FilteredTrackIdList - Reactive smart-list membership derived from a source list.
   * Evaluates track membership using a compiled ExecutionPlan.
   *
   * Compile-once evaluation: recompiles only when expression changes,
   * re-evaluates incrementally when source tracks are updated.
   *
   * Uses multiple inheritance: TrackIdList (to be a list) + TrackIdListObserver (to observe source).
   */
  class FilteredTrackIdList final
    : public TrackIdList
    , public TrackIdListObserver
  {
  public:
    explicit FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& ml);
    ~FilteredTrackIdList() override;

    void setExpression(std::string expr);
    void reload();

    // TrackIdList interface
    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] TrackId trackIdAt(std::size_t index) const override;
    [[nodiscard]] std::optional<std::size_t> indexOf(TrackId id) const override;

    bool hasError() const { return _hasError; }
    std::string const& errorMessage() const { return _errorMessage; }

  private:
    // TrackIdListObserver interface (inherited)
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void rebuild();
    bool evaluate(TrackId id) const;

    TrackIdList& _source;
    rs::core::MusicLibrary* _ml;
    std::vector<TrackId> _filteredIds;

    // Expression state
    std::string _expression;
    bool _hasError = false;
    std::string _errorMessage;

    // Compiled plan (rebuilt on expression change)
    std::unique_ptr<rs::expr::ExecutionPlan> _plan;
    std::unique_ptr<rs::expr::PlanEvaluator> _evaluator;
  };

} // namespace app::model