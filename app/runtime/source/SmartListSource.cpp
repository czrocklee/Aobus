// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/Log.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

namespace ao::rt
{
  SmartListSource::SmartListSource(TrackSource& source, library::MusicLibrary& ml, SmartListEvaluator& evaluator)
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
    if (_evaluator == nullptr || !_evaluator->isAlive())
    {
      return;
    }

    _evaluator->rebuild(*this);
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
    if (_evaluator == nullptr || !_evaluator->isAlive())
    {
      return;
    }

    _evaluator->notifyUpdated(_source, id);
  }

  void SmartListSource::stageExpression(std::string expr)
  {
    _staged.expression = std::move(expr);
    _dirty = true;

    auto const stageError = [this](Error error)
    {
      APP_LOG_ERROR("Smart list expression error for '{}': {}", _staged.expression, error.message);

      _staged.optError = std::move(error);
      _staged.planPtr.reset();
    };

    auto parsed = query::parse(_staged.expression.empty() ? "true" : _staged.expression);

    if (!parsed)
    {
      stageError(std::move(parsed).error());
      return;
    }

    auto plan = query::compileQuery(*parsed, &_ml.dictionary());

    if (!plan)
    {
      stageError(std::move(plan).error());
      return;
    }

    _staged.planPtr = std::make_unique<query::ExecutionPlan>(*std::move(plan));
    _staged.optError.reset();
  }

  void SmartListSource::applyStagedState()
  {
    if (!_dirty)
    {
      return;
    }

    _current = std::move(_staged);
    _dirty = false;
  }
} // namespace ao::rt
