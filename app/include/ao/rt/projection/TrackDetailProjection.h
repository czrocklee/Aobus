// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../TrackField.h"
#include "../TrackFieldValue.h"
#include "../ViewIds.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt
{
  enum class SelectionKind : std::uint8_t
  {
    None,
    Single,
    Multiple,
  };

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
    std::uint64_t revision = 0;

    ResourceId singleCoverArtId{kInvalidResourceId};
    std::array<AggregateValue<TrackFieldRawValue>, kTrackFieldCount> fields{};
    std::vector<CustomMetadataItem> customMetadata{};
    std::vector<DictionaryId> commonTagIds{};
  };

  struct FocusedViewTarget final
  {};
  struct ExplicitViewTarget final
  {
    ViewId viewId{};
  };
  struct ExplicitSelectionTarget final
  {
    std::vector<TrackId> trackIds{};
  };

  using DetailTarget = std::variant<FocusedViewTarget, ExplicitViewTarget, ExplicitSelectionTarget>;

  class TrackDetailProjection
  {
  public:
    virtual ~TrackDetailProjection() = default;

    TrackDetailProjection(TrackDetailProjection const&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection const&) = delete;
    TrackDetailProjection(TrackDetailProjection&&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection&&) = delete;

    virtual TrackDetailSnapshot snapshot() const = 0;
    virtual async::Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler) = 0;

  protected:
    TrackDetailProjection() = default;
  };
} // namespace ao::rt
