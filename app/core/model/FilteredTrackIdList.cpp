// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/model/FilteredTrackIdList.h"

#include "core/Log.h"
#include "core/model/SmartListEngine.h"
#include "core/model/TrackIdList.h"

#include <rs/expr/Parser.h>

#include <algorithm>

namespace app::core::model
{

  FilteredTrackIdList::FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& ml, SmartListEngine& engine)
    : _source{&source}, _ml{&ml}, _engine{&engine}
  {
    _engine->registerList(source, *this);
    stageExpression("");
  }

  FilteredTrackIdList::~FilteredTrackIdList()
  {
    if (_engine != nullptr && _engine->isAlive())
    {
      _engine->unregisterList(*_source, *this);
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

  void FilteredTrackIdList::notifyTrackDataChanged(TrackId id)
  {
    if (_engine != nullptr && _engine->isAlive())
    {
      _engine->notifyTrackDataChanged(*_source, id);
    }
  }

  void FilteredTrackIdList::stageExpression(std::string expr)
  {
    _stagedExpression = std::move(expr);

    try
    {
      auto parsed = _stagedExpression.empty() ? rs::expr::parse("true") : rs::expr::parse(_stagedExpression);
      auto compiler = rs::expr::QueryCompiler{&_ml->dictionary()};
      _stagedPlan = std::make_unique<rs::expr::ExecutionPlan>(compiler.compile(parsed));
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

} // namespace app::core::model
