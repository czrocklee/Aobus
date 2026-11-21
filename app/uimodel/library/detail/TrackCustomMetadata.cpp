// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackCustomMetadata.h>

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  std::string formatTrackCustomMetadataDisplayText(rt::CustomMetadataItem const& item, std::string_view const mixedText)
  {
    if (item.value.mixed)
    {
      return std::string{mixedText};
    }

    return item.value.optValue.value_or("");
  }

  bool isProtectedTrackCustomMetadataEditText(std::string_view const newText, std::string_view const mixedText)
  {
    return newText == mixedText;
  }

  CustomMetadataAddValidation validateCustomMetadataAddition(rt::TrackDetailSnapshot const& snap,
                                                             std::string_view const key)
  {
    if (std::ranges::any_of(
          snap.customMetadata, [key](rt::CustomMetadataItem const& item) { return std::string_view{item.key} == key; }))
    {
      return CustomMetadataAddValidation::DuplicateCustomMetadata;
    }

    if (rt::isReservedCustomMetadataKey(key))
    {
      return CustomMetadataAddValidation::ReservedKey;
    }

    return CustomMetadataAddValidation::Accepted;
  }

  void setCustomMetadataPatchEntry(rt::MetadataPatch& patch,
                                   std::string_view const key,
                                   std::optional<std::string> optValue)
  {
    patch.customUpdates.insert_or_assign(std::string{key}, std::move(optValue));
  }

  rt::MetadataPatch makeCustomMetadataUpdatePatch(std::string_view const key, std::string_view const value)
  {
    auto patch = rt::MetadataPatch{};
    setCustomMetadataPatchEntry(patch, key, std::string{value});
    return patch;
  }

  rt::MetadataPatch makeCustomMetadataDeletePatch(std::string_view const key)
  {
    auto patch = rt::MetadataPatch{};
    setCustomMetadataPatchEntry(patch, key, std::nullopt);
    return patch;
  }
} // namespace ao::uimodel
