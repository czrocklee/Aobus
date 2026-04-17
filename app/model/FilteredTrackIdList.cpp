// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "FilteredTrackIdList.h"

#include "SmartListEngine.h"
#include "TrackIdList.h"

namespace app::model
{

  FilteredTrackIdList::FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& /*ml*/, SmartListEngine& engine)
    : _engine{&engine}
  {
    _registrationId = _engine->registerList(source, *this);
  }

  FilteredTrackIdList::~FilteredTrackIdList()
  {
    // Only unregister if engine is still alive (not being destroyed)
    // This prevents use-after-free when the engine is destroyed before the facade
    if (_engine && _engine->isAlive() && _registrationId != 0)
    {
      _engine->unregisterList(_registrationId);
    }
  }

  void FilteredTrackIdList::setExpression(std::string expr)
  {
    if (_engine && _registrationId != 0)
    {
      _engine->setExpression(_registrationId, std::move(expr));
    }
  }

  void FilteredTrackIdList::reload()
  {
    if (_engine && _registrationId != 0)
    {
      _engine->rebuild(_registrationId);
    }
  }

  std::size_t FilteredTrackIdList::size() const
  {
    if (!_engine || _registrationId == 0)
    {
      return 0;
    }
    return _engine->size(_registrationId);
  }

  TrackId FilteredTrackIdList::trackIdAt(std::size_t index) const
  {
    if (!_engine || _registrationId == 0)
    {
      throw std::out_of_range("Invalid state");
    }
    return _engine->trackIdAt(_registrationId, index);
  }

  std::optional<std::size_t> FilteredTrackIdList::indexOf(TrackId id) const
  {
    if (!_engine || _registrationId == 0)
    {
      return std::nullopt;
    }
    return _engine->indexOf(_registrationId, id);
  }

  bool FilteredTrackIdList::hasError() const
  {
    if (!_engine || _registrationId == 0)
    {
      return true;
    }
    return _engine->hasError(_registrationId);
  }

  std::string const& FilteredTrackIdList::errorMessage() const
  {
    static std::string const empty;
    if (!_engine || _registrationId == 0)
    {
      return empty;
    }
    return _engine->errorMessage(_registrationId);
  }

  void FilteredTrackIdList::notifyEngineReset()
  {
    TrackIdList::notifyReset();
  }

  void FilteredTrackIdList::notifyEngineInserted(TrackId id, std::size_t index)
  {
    TrackIdList::notifyInserted(id, index);
  }

  void FilteredTrackIdList::notifyEngineUpdated(TrackId id, std::size_t index)
  {
    TrackIdList::notifyUpdated(id, index);
  }

  void FilteredTrackIdList::notifyEngineRemoved(TrackId id, std::size_t index)
  {
    TrackIdList::notifyRemoved(id, index);
  }

} // namespace app::model