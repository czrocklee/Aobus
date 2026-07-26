// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../TrackField.h"
#include "../TrackFieldValue.h"
#include <ao/CoreIds.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  enum class SelectionKind : std::uint8_t
  {
    None,
    Single,
    Multiple,
  };

  // One field read across a selection: a value when every track agrees, or the
  // mixed flag when they do not.
  template<typename T>
  struct AggregateValue final
  {
    std::optional<T> optValue{};
    bool mixed = false;
  };

  struct CustomMetadataItem final
  {
    std::string key{};
    AggregateValue<std::string> value{};
    bool presentOnAll = false;
    bool presentOnAny = false;
  };

  struct TrackDetailSnapshot final
  {
    SelectionKind selectionKind = SelectionKind::None;
    std::vector<TrackId> trackIds{};

    ResourceId singleCoverArtId{kInvalidResourceId};
    std::array<AggregateValue<TrackFieldRawValue>, kTrackFieldCount> fields{};
    std::vector<CustomMetadataItem> customMetadata{};
    std::vector<DictionaryId> commonTagIds{};
  };
} // namespace ao::rt
