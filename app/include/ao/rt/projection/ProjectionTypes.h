// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "../CorePrimitives.h"
#include "../TrackField.h"
#include "../TrackFieldValue.h"
#include "../TrackPresentation.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <boost/container/small_vector.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt
{
  struct TrackGroupSectionSnapshot final
  {
    Range rows{};
    std::string primaryText{};
    std::string secondaryText{};
    std::string tertiaryText{};
    ResourceId imageId{kInvalidResourceId};
  };

  struct ProjectionReset final
  {
    std::uint64_t revision = 0;
  };

  struct ProjectionInsertRange final
  {
    Range range{};
  };

  struct ProjectionRemoveRange final
  {
    Range range{};
  };

  struct ProjectionUpdateRange final
  {
    Range range{};
  };

  using TrackListProjectionDelta =
    std::variant<ProjectionReset, ProjectionInsertRange, ProjectionRemoveRange, ProjectionUpdateRange>;

  struct TrackListProjectionDeltaBatch final
  {
    std::uint64_t revision = 0;

    // Nearly every publish carries exactly one delta, so a small_vector with inline
    // capacity for one element keeps the common case allocation-free. Larger batches
    // (rare) spill to the heap transparently.
    boost::container::small_vector<TrackListProjectionDelta, 1> deltas{};
  };

  class ITrackListProjection
  {
  public:
    virtual ~ITrackListProjection() = default;

    ITrackListProjection(ITrackListProjection const&) = delete;
    ITrackListProjection& operator=(ITrackListProjection const&) = delete;
    ITrackListProjection(ITrackListProjection&&) = delete;
    ITrackListProjection& operator=(ITrackListProjection&&) = delete;

    virtual ViewId viewId() const noexcept = 0;
    virtual std::uint64_t revision() const noexcept = 0;

    virtual TrackPresentationSpec presentation() const = 0;
    virtual std::size_t groupCount() const noexcept = 0;
    virtual TrackGroupSectionSnapshot groupAt(std::size_t groupIndex) const = 0;
    virtual std::optional<std::size_t> groupIndexAt(std::size_t rowIndex) const = 0;

    virtual std::size_t size() const noexcept = 0;
    virtual TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(TrackId trackId) const noexcept = 0;

    virtual Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) = 0;

  protected:
    ITrackListProjection() = default;
  };

  struct TrackListProjectionChanged final
  {
    ViewId viewId{};
    std::shared_ptr<ITrackListProjection> projectionPtr{};
    std::uint64_t revision = 0;
  };

  struct FilterStatusChanged final
  {
    ViewId viewId{};
    std::string expression{};
    bool pending = false;
    std::optional<Error> optError = std::nullopt;
    std::uint64_t revision = 0;
  };

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

  class ITrackDetailProjection
  {
  public:
    virtual ~ITrackDetailProjection() = default;

    ITrackDetailProjection(ITrackDetailProjection const&) = delete;
    ITrackDetailProjection& operator=(ITrackDetailProjection const&) = delete;
    ITrackDetailProjection(ITrackDetailProjection&&) = delete;
    ITrackDetailProjection& operator=(ITrackDetailProjection&&) = delete;

    virtual TrackDetailSnapshot snapshot() const = 0;
    virtual Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler) = 0;

  protected:
    ITrackDetailProjection() = default;
  };
} // namespace ao::rt
