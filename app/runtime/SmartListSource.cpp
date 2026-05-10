// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SmartListSource.h"
#include "SmartListEvaluator.h"

#include <ao/query/Parser.h>
#include <ao/utility/Log.h>

#include <algorithm>

namespace ao::app
{
  SmartListSource::SmartListSource(TrackSource& source, ao::library::MusicLibrary& ml, SmartListEvaluator& evaluator)
    : _source{source}, _ml{ml}, _evaluator{&evaluator}
  {
    _evaluator->registerList(_source, *this);
    stageExpression("");
  }

  SmartListSource::~SmartListSource()
  {
    if (_evaluator != nullptr && _evaluator->isAlive())
    {
      _evaluator->unregisterList(_source, *this);
    }
  }

  void SmartListSource::setExpression(std::string expr)
  {
    stageExpression(std::move(expr));
  }

  void SmartListSource::reload()
  {
    if (_evaluator != nullptr && _evaluator->isAlive())
    {
      _evaluator->rebuild(*this);
    }
  }

  std::optional<std::size_t> SmartListSource::indexOf(TrackId id) const
  {
    auto const it = _members.find(id);

    if (it == _members.end())
    {
      return std::nullopt;
    }

    return static_cast<std::size_t>(std::distance(_members.begin(), it));
  }

  void SmartListSource::notifyUpdated(TrackId id)
  {
    if (_evaluator != nullptr && _evaluator->isAlive())
    {
      _evaluator->notifyUpdated(_source, id);
    }
  }

  void SmartListSource::stageExpression(std::string expr)
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
      APP_LOG_ERROR("Smart list expression error for '{}': {}", _stagedExpression, e.what());

      _stagedHasError = true;
      _stagedErrorMessage = e.what();
      _stagedPlan.reset();
    }

    _dirty = true;
  }

  void SmartListSource::applyStagedState()
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
}
