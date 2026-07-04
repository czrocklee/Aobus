// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCustomMetadataWorkflow.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  std::string displayTextForTrackCustomMetadata(rt::CustomMetadataItem const& item)
  {
    if (item.value.mixed)
    {
      return std::string{kMultipleTrackValuesText};
    }

    return item.value.optValue.value_or("");
  }

  bool isProtectedTrackCustomMetadataEditText(std::string_view const newText)
  {
    return newText == kMultipleTrackValuesText;
  }

  CustomMetadataAddValidation validateCustomMetadataAddition(rt::TrackDetailSnapshot const& snap,
                                                             std::string_view const key)
  {
    if (std::ranges::any_of(
          snap.customMetadata, [key](rt::CustomMetadataItem const& item) { return std::string_view{item.key} == key; }))
    {
      return CustomMetadataAddValidation::DuplicateCustomMetadata;
    }

    if (rt::trackFieldFromId(key))
    {
      return CustomMetadataAddValidation::ReservedTrackField;
    }

    return CustomMetadataAddValidation::Accepted;
  }

  std::optional<std::string> undoValueForDeletedTrackCustomMetadata(rt::TrackDetailSnapshot const& snap,
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

  rt::MetadataPatch makeCustomMetadataUpdatePatch(std::string_view const key, std::string_view const value)
  {
    auto patch = rt::MetadataPatch{};
    patch.customUpdates[std::string{key}] = std::string{value};
    return patch;
  }

  rt::MetadataPatch makeCustomMetadataDeletePatch(std::string_view const key)
  {
    auto patch = rt::MetadataPatch{};
    patch.customUpdates[std::string{key}] = std::nullopt;
    return patch;
  }
} // namespace ao::uimodel
