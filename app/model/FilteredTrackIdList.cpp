// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "FilteredTrackIdList.h"

#include "TrackIdList.h"

#include <algorithm>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/lmdb/Transaction.h>

namespace app::model
{

  FilteredTrackIdList::FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& ml)
    : _source{source}, _ml{&ml}
  {
    _source.attach(this);
  }

  FilteredTrackIdList::~FilteredTrackIdList()
  {
    _source.detach(this);
  }

  void FilteredTrackIdList::setExpression(std::string expr)
  {
    _expression = std::move(expr);

    try
    {
      auto parsedExpr = rs::expr::parse(_expression);
      rs::expr::QueryCompiler compiler;
      _plan = std::make_unique<rs::expr::ExecutionPlan>(compiler.compile(parsedExpr));
      _evaluator = std::make_unique<rs::expr::PlanEvaluator>();
      _hasError = false;
      _errorMessage.clear();
    }
    catch (std::exception const& e)
    {
      _hasError = true;
      _errorMessage = e.what();
      _plan.reset();
      _evaluator.reset();
    }
  }

  void FilteredTrackIdList::reload()
  {
    rebuild();
  }

  std::size_t FilteredTrackIdList::size() const
  {
    return _filteredIds.size();
  }

  TrackId FilteredTrackIdList::trackIdAt(std::size_t index) const
  {
    return _filteredIds.at(index);
  }

  std::optional<std::size_t> FilteredTrackIdList::indexOf(TrackId id) const
  {
    auto const it = std::find(_filteredIds.begin(), _filteredIds.end(), id);
    if (it != _filteredIds.end()) { return static_cast<std::size_t>(std::distance(_filteredIds.begin(), it)); }
    return std::nullopt;
  }

  void FilteredTrackIdList::rebuild()
  {
    if (_hasError || !_plan || !_evaluator)
    {
      _filteredIds.clear();
      TrackIdList::notifyReset();
      return;
    }

    _filteredIds.clear();
    for (std::size_t i = 0; i < _source.size(); ++i)
    {
      auto const id = _source.trackIdAt(i);
      if (evaluate(id)) { _filteredIds.push_back(id); }
    }
    TrackIdList::notifyReset();
  }

  bool FilteredTrackIdList::evaluate(TrackId id) const
  {
    if (!_plan || !_evaluator) { return false; }

    // Determine load mode based on access profile
    auto const loadMode = (_plan->accessProfile == rs::expr::AccessProfile::HotOnly)
                            ? rs::core::TrackStore::Reader::LoadMode::Hot
                            : rs::core::TrackStore::Reader::LoadMode::Both;

    // Load track view for evaluation
    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _ml->tracks().reader(txn);
    auto const optView = reader.get(id, loadMode);

    if (!optView)
    {
      return false; // Track not found
    }

    return _evaluator->matches(*_plan, *optView);
  }

  void FilteredTrackIdList::onReset()
  {
    rebuild();
  }

  void FilteredTrackIdList::onInserted(TrackId id, std::size_t /*index*/)
  {
    if (_hasError) { return; }

    // Check if this track matches the filter
    bool const matches = evaluate(id);
    if (matches)
    {
      auto const insertPos = std::lower_bound(_filteredIds.begin(), _filteredIds.end(), id);
      auto const insertIndex = static_cast<std::size_t>(std::distance(_filteredIds.begin(), insertPos));
      _filteredIds.insert(insertPos, id);
      TrackIdList::notifyInserted(id, insertIndex);
    }
  }

  void FilteredTrackIdList::onUpdated(TrackId id, std::size_t /*index*/)
  {
    if (_hasError) { return; }

    // Re-evaluate the track
    bool const nowMatches = evaluate(id);
    bool const wasInList = std::find(_filteredIds.begin(), _filteredIds.end(), id) != _filteredIds.end();

    if (nowMatches && !wasInList)
    {
      // Track now matches but wasn't in list - add it
      auto const insertPos = std::lower_bound(_filteredIds.begin(), _filteredIds.end(), id);
      auto const insertIndex = static_cast<std::size_t>(std::distance(_filteredIds.begin(), insertPos));
      _filteredIds.insert(insertPos, id);
      TrackIdList::notifyInserted(id, insertIndex);
    }
    else if (!nowMatches && wasInList)
    {
      // Track no longer matches but was in list - remove it
      auto const it = std::find(_filteredIds.begin(), _filteredIds.end(), id);
      if (it != _filteredIds.end())
      {
        auto const removeIndex = static_cast<std::size_t>(std::distance(_filteredIds.begin(), it));
        _filteredIds.erase(it);
        TrackIdList::notifyRemoved(id, removeIndex);
      }
    }
    else if (nowMatches && wasInList)
    {
      // Track still matches - notify update
      auto const it = std::find(_filteredIds.begin(), _filteredIds.end(), id);
      if (it != _filteredIds.end())
      {
        auto const updateIndex = static_cast<std::size_t>(std::distance(_filteredIds.begin(), it));
        TrackIdList::notifyUpdated(id, updateIndex);
      }
    }
  }

  void FilteredTrackIdList::onRemoved(TrackId id, std::size_t /*index*/)
  {
    auto const it = std::find(_filteredIds.begin(), _filteredIds.end(), id);
    if (it != _filteredIds.end())
    {
      auto const removeIndex = static_cast<std::size_t>(std::distance(_filteredIds.begin(), it));
      _filteredIds.erase(it);
      TrackIdList::notifyRemoved(id, removeIndex);
    }
  }

} // namespace app::model