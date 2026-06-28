// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/StateTypes.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  enum class TrackCustomPropertyAddValidation : std::uint8_t
  {
    Accepted,
    DuplicateCustomProperty,
    ReservedTrackField,
  };

  std::string displayTextForTrackCustomProperty(rt::CustomMetadataItem const& item);
  bool isProtectedTrackCustomPropertyEditText(std::string_view newText);

  TrackCustomPropertyAddValidation validateTrackCustomPropertyAddition(rt::TrackDetailSnapshot const& snap,
                                                                       std::string_view key);
  std::optional<std::string> undoValueForDeletedTrackCustomProperty(rt::TrackDetailSnapshot const& snap,
                                                                    std::string_view key);

  rt::MetadataPatch makeTrackCustomPropertyUpdatePatch(std::string_view key, std::string_view value);
  rt::MetadataPatch makeTrackCustomPropertyDeletePatch(std::string_view key);
} // namespace ao::uimodel::track
