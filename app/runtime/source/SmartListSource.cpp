// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/source/SmartListSource.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>
#include <ao/rt/Log.h>
#include <ao/rt/source/SmartListEvaluator.h>
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

    auto parsedRes = query::parse(_optPending->expression.empty() ? "true" : _optPending->expression);

    if (!parsedRes)
    {
      setError(std::move(parsedRes).error());
      return;
    }

    auto planRes = query::compileQuery(*parsedRes);

    if (!planRes)
    {
      setError(std::move(planRes).error());
      return;
    }

    _optPending->planPtr = std::make_unique<query::ExecutionPlan>(*std::move(planRes));
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

  void SmartListSource::discardSnapshot() noexcept
  {
    _members.clear();
  }
} // namespace ao::rt
