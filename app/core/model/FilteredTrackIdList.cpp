// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/model/FilteredTrackIdList.h"

#include "core/model/SmartListEngine.h"
#include "core/model/TrackIdList.h"

namespace app::core::model
{

  FilteredTrackIdList::FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& /*ml*/, SmartListEngine& engine)
    : _engine{&engine}
  {
    _registrationId = _engine->registerList(source, *this);
  }

  SmartListEngine* FilteredTrackIdList::engine() const
  {
    return (_engine && _engine->isAlive() && _registrationId != 0) ? _engine : nullptr;
  }

  FilteredTrackIdList::~FilteredTrackIdList()
  {
    if (auto* e = engine()) e->unregisterList(_registrationId);
  }

  void FilteredTrackIdList::setExpression(std::string expr)
  {
    if (auto* e = engine()) e->setExpression(_registrationId, std::move(expr));
  }

  void FilteredTrackIdList::reload()
  {
    if (auto* e = engine()) e->rebuild(_registrationId);
  }

  std::size_t FilteredTrackIdList::size() const
  {
    auto* e = engine();
    return e ? e->size(_registrationId) : 0;
  }

  TrackId FilteredTrackIdList::trackIdAt(std::size_t index) const
  {
    auto* e = engine();
    if (!e) throw std::out_of_range("Invalid state");
    return e->trackIdAt(_registrationId, index);
  }

  std::optional<std::size_t> FilteredTrackIdList::indexOf(TrackId id) const
  {
    auto* e = engine();
    return e ? e->indexOf(_registrationId, id) : std::nullopt;
  }

  bool FilteredTrackIdList::hasError() const
  {
    auto* e = engine();
    return e ? e->hasError(_registrationId) : true;
  }

  std::string const& FilteredTrackIdList::errorMessage() const
  {
    static std::string const empty;
    auto* e = engine();
    return e ? e->errorMessage(_registrationId) : empty;
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

  void FilteredTrackIdList::notifyTrackDataChanged(TrackId id)
  {
    if (auto* e = engine()) e->notifyTrackDataChanged(_registrationId, id);
  }

} // namespace app::core::model