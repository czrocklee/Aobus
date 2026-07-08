// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  enum class CustomMetadataAddValidation : std::uint8_t
  {
    Accepted,
    DuplicateCustomMetadata,
    ReservedTrackField,
  };

  std::string formatTrackCustomMetadataDisplayText(rt::CustomMetadataItem const& item);
  bool isProtectedTrackCustomMetadataEditText(std::string_view newText);

  CustomMetadataAddValidation validateCustomMetadataAddition(rt::TrackDetailSnapshot const& snap, std::string_view key);
  std::optional<std::string> undoValueForDeletedTrackCustomMetadata(rt::TrackDetailSnapshot const& snap,
                                                                    std::string_view key);

  rt::MetadataPatch makeCustomMetadataUpdatePatch(std::string_view key, std::string_view value);
  rt::MetadataPatch makeCustomMetadataDeletePatch(std::string_view key);
} // namespace ao::uimodel
