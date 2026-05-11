// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include "CorePrimitives.h"
#include "StateTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt
{
  struct TrackListPresentationSnapshot final
  {
    TrackGroupKey groupBy = TrackGroupKey::None;
    std::vector<TrackSortTerm> effectiveSortBy{};
    std::vector<TrackSortField> redundantFields{};
    std::uint64_t revision = 0;
  };

  struct TrackGroupSectionSnapshot final
  {
    Range rows{};
    std::string label{};
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
    std::vector<TrackListProjectionDelta> deltas{};
  };

  class ITrackListProjection
  {
  public:
    virtual ~ITrackListProjection() = default;

    virtual ViewId viewId() const noexcept = 0;
    virtual std::uint64_t revision() const noexcept = 0;

    virtual TrackListPresentationSnapshot presentation() const = 0;
    virtual std::size_t groupCount() const noexcept = 0;
    virtual TrackGroupSectionSnapshot groupAt(std::size_t groupIndex) const = 0;
    virtual std::optional<std::size_t> groupIndexAt(std::size_t rowIndex) const = 0;

    virtual std::size_t size() const noexcept = 0;
    virtual ao::TrackId trackIdAt(std::size_t index) const = 0;
    virtual std::optional<std::size_t> indexOf(ao::TrackId trackId) const noexcept = 0;

    virtual Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&)> handler) = 0;
  };

  enum class SelectionKind : std::uint8_t
  {
    None,
    Single,
    Multiple,
  };

  template<class T>
  struct AggregateValue final
  {
    std::optional<T> optValue{};
    bool mixed = false;
  };

  struct AudioPropertySnapshot final
  {
    AggregateValue<std::uint16_t> codecId{};
    AggregateValue<std::uint32_t> sampleRate{};
    AggregateValue<std::uint8_t> channels{};
    AggregateValue<std::uint8_t> bitDepth{};
    AggregateValue<std::uint32_t> durationMs{};
  };

  struct TrackDetailSnapshot final
  {
    SelectionKind selectionKind = SelectionKind::None;
    std::vector<ao::TrackId> trackIds{};
    std::uint64_t revision = 0;

    AggregateValue<std::string> title{};
    AggregateValue<std::string> artist{};
    AggregateValue<std::string> album{};

    ao::ResourceId singleCoverArtId{};
    AudioPropertySnapshot audio{};
    std::vector<ao::DictionaryId> commonTagIds{};
  };

  struct FocusedViewTarget final
  {};
  struct ExplicitViewTarget final
  {
    ViewId viewId{};
  };
  struct ExplicitSelectionTarget final
  {
    std::vector<ao::TrackId> trackIds{};
  };

  using DetailTarget = std::variant<FocusedViewTarget, ExplicitViewTarget, ExplicitSelectionTarget>;

  class ITrackDetailProjection
  {
  public:
    virtual ~ITrackDetailProjection() = default;

    virtual TrackDetailSnapshot snapshot() const = 0;
    virtual Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler) = 0;
  };
}
