// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/track/TrackCustomPropertyWorkflow.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  std::string displayTextForTrackCustomProperty(rt::CustomMetadataItem const& item)
  {
    if (item.value.mixed)
    {
      return std::string{kMultipleTrackValuesText};
    }

    return item.value.optValue.value_or("");
  }

  bool isProtectedTrackCustomPropertyEditText(std::string_view const newText)
  {
    return newText == kMultipleTrackValuesText;
  }

  TrackCustomPropertyAddValidation validateTrackCustomPropertyAddition(rt::TrackDetailSnapshot const& snap,
                                                                       std::string_view const key)
  {
    if (std::ranges::any_of(
          snap.customMetadata, [key](rt::CustomMetadataItem const& item) { return std::string_view{item.key} == key; }))
    {
      return TrackCustomPropertyAddValidation::DuplicateCustomProperty;
    }

    if (rt::trackFieldFromId(key))
    {
      return TrackCustomPropertyAddValidation::ReservedTrackField;
    }

    return TrackCustomPropertyAddValidation::Accepted;
  }

  std::optional<std::string> undoValueForDeletedTrackCustomProperty(rt::TrackDetailSnapshot const& snap,
                                                                    std::string_view const key)
  {
    for (auto const& item : snap.customMetadata)
    {
      if (std::string_view{item.key} == key && item.presentOnAll && !item.value.mixed)
      {
        return item.value.optValue.value_or("");
      }
    }

    return std::nullopt;
  }

  rt::MetadataPatch makeTrackCustomPropertyUpdatePatch(std::string_view const key, std::string_view const value)
  {
    auto patch = rt::MetadataPatch{};
    patch.customUpdates[std::string{key}] = std::string{value};
    return patch;
  }

  rt::MetadataPatch makeTrackCustomPropertyDeletePatch(std::string_view const key)
  {
    auto patch = rt::MetadataPatch{};
    patch.customUpdates[std::string{key}] = std::nullopt;
    return patch;
  }
} // namespace ao::uimodel::track
