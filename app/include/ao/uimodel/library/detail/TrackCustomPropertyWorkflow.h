// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  enum class CustomPropertyAddValidation : std::uint8_t
  {
    Accepted,
    DuplicateCustomProperty,
    ReservedTrackField,
  };

  std::string displayTextForTrackCustomProperty(rt::CustomMetadataItem const& item);
  bool isProtectedTrackCustomPropertyEditText(std::string_view newText);

  CustomPropertyAddValidation validateCustomPropertyAddition(rt::TrackDetailSnapshot const& snap, std::string_view key);
  std::optional<std::string> undoValueForDeletedTrackCustomProperty(rt::TrackDetailSnapshot const& snap,
                                                                    std::string_view key);

  rt::MetadataPatch makeCustomPropertyUpdatePatch(std::string_view key, std::string_view value);
  rt::MetadataPatch makeTrackCustomPropertyDeletePatch(std::string_view key);
} // namespace ao::uimodel
