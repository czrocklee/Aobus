// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackPresentationCatalog.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>
#include <ao/uimodel/track/TrackPresentationRecommender.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  TrackPresentationPreferenceStore::TrackPresentationPreferenceStore(TrackPresentationCatalog& catalog)
    : _catalog{catalog}
  {
  }

  void TrackPresentationPreferenceStore::setListPresentations(std::map<ListId, std::string> const& presentations)
  {
    if (_presentations == presentations)
    {
      return;
    }

    _presentations = presentations;
    _changed.emit(kInvalidListId);
  }

  std::optional<std::string_view> TrackPresentationPreferenceStore::presentationIdForList(ListId listId) const
  {
    if (auto const it = _presentations.find(listId); it != _presentations.end())
    {
      return it->second;
    }

    return std::nullopt;
  }

  void TrackPresentationPreferenceStore::setPresentationIdForList(ListId listId, std::string_view presentationId)
  {
    if (listId == kInvalidListId)
    {
      return;
    }

    auto const strId = std::string{presentationId};

    if (strId.empty())
    {
      clearPresentationForList(listId);
      return;
    }

    if (auto const it = _presentations.find(listId); it != _presentations.end() && it->second == strId)
    {
      return;
    }

    _presentations[listId] = strId;
    _changed.emit(listId);
  }

  void TrackPresentationPreferenceStore::clearPresentationForList(ListId listId)
  {
    if (_presentations.erase(listId) > 0)
    {
      _changed.emit(listId);
    }
  }

  rt::TrackPresentationSpec TrackPresentationPreferenceStore::presentationForList(
    ListId listId,
    std::string_view smartListFilter) const
  {
    if (auto const optId = presentationIdForList(listId); optId)
    {
      if (auto const optSpec = _catalog.specForId(*optId); optSpec)
      {
        return *optSpec;
      }

      APP_LOG_DEBUG(
        "TrackPresentationPreferenceStore: saved presentation id '{}' is unavailable; using recommendation fallback",
        *optId);
    }

    return recommendPresentation(smartListFilter, _catalog.builtinPresets(), _catalog.customPresentations());
  }
} // namespace ao::uimodel::track
