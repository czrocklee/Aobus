// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/model/FilteredTrackIdList.h>
#include <ao/utility/Log.h>

#include <ao/model/SmartListEngine.h>
#include <ao/model/TrackIdList.h>
#include <iostream>

#include <ao/query/Parser.h>

#include <algorithm>

namespace ao::model
{
  FilteredTrackIdList::FilteredTrackIdList(TrackIdList& source, ao::library::MusicLibrary& ml, SmartListEngine& engine)
    : _source{source}, _ml{ml}, _engine{&engine}
  {
    _engine->registerList(_source, *this);
    stageExpression("");
  }

  FilteredTrackIdList::~FilteredTrackIdList()
  {
    if (_engine != nullptr && _engine->isAlive())
    {
      _engine->unregisterList(_source, *this);
    }
  }

  void FilteredTrackIdList::setExpression(std::string expr)
  {
    stageExpression(std::move(expr));
  }

  void FilteredTrackIdList::reload()
  {
    if (_engine != nullptr && _engine->isAlive())
    {
      _engine->rebuild(*this);
    }
  }

  std::optional<std::size_t> FilteredTrackIdList::indexOf(TrackId id) const
  {
    auto const it = _members.find(id);

    if (it == _members.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::distance(_members.begin(), it));
  }

  void FilteredTrackIdList::notifyUpdated(TrackId id)
  {
    if (_engine != nullptr && _engine->isAlive())
    {
      _engine->notifyUpdated(_source, id);
    }
  }

  void FilteredTrackIdList::stageExpression(std::string expr)
  {
    _stagedExpression = std::move(expr);

    try
    {
      auto parsed = _stagedExpression.empty() ? ao::query::parse("true") : ao::query::parse(_stagedExpression);
      auto compiler = ao::query::QueryCompiler{&_ml.dictionary()};
      _stagedPlan = std::make_unique<ao::query::ExecutionPlan>(compiler.compile(parsed));
      _stagedHasError = false;
      _stagedErrorMessage.clear();
    }
    catch (std::exception const& e)
    {
      std::cerr << "Smart list expression error for '" << _stagedExpression << "': " << e.what() << '\n';
      _stagedHasError = true;
      _stagedErrorMessage = e.what();
      _stagedPlan.reset();
    }

    _dirty = true;
  }

  void FilteredTrackIdList::applyStagedState()
  {
    if (!_dirty)
    {
      return;
    }

    _expression = std::move(_stagedExpression);
    _hasError = _stagedHasError;
    _errorMessage = std::move(_stagedErrorMessage);
    _plan = std::move(_stagedPlan);
    _dirty = false;
  }
} // namespace ao::model
