// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/Log.h>
#include <ao/rt/source/SmartListEvaluator.h>
#include <ao/rt/source/SmartListSource.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::rt
{
  SmartListSource::SmartListSource(TrackSourceLease sourceLease, SmartListEvaluator& evaluator)
    : _sourceLease{std::move(sourceLease)}, _evaluator{&evaluator}
  {
    setExpression("");
    _evaluator->registerList(*this);
  }

  SmartListSource::~SmartListSource()
  {
    if (_evaluator != nullptr && _evaluator->isAlive())
    {
      _evaluator->unregisterList(*this);
    }
  }

  void SmartListSource::setExpression(std::string expr)
  {
    _optPending.emplace();
    _optPending->expression = std::move(expr);

    auto const setError = [this](Error error)
    {
      APP_LOG_ERROR("Smart list expression error for '{}': {}", _optPending->expression, error.message);

      _optPending->optError = std::move(error);
      _optPending->planPtr.reset();
    };

    auto parsed = query::parse(_optPending->expression.empty() ? "true" : _optPending->expression);

    if (!parsed)
    {
      setError(std::move(parsed).error());
      return;
    }

    auto plan = query::compileQuery(*parsed);

    if (!plan)
    {
      setError(std::move(plan).error());
      return;
    }

    _optPending->planPtr = std::make_unique<query::ExecutionPlan>(*std::move(plan));
    _optPending->optError.reset();
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
    return _members.indexOf(id);
  }

  void SmartListSource::notifyUpdated(TrackId id)
  {
    if (_evaluator == nullptr || !_evaluator->isAlive())
    {
      return;
    }

    _evaluator->notifyUpdated(*this, id);
  }

  void SmartListSource::applyPendingState()
  {
    if (!_optPending)
    {
      return;
    }

    _current = std::move(*_optPending);
    _optPending.reset();
  }

  void SmartListSource::replaceMembers(std::vector<TrackId> members)
  {
    _members.assign(members);
  }
} // namespace ao::rt
