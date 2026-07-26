// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../PlaybackLaunchSpec.h"
#include "../TrackPresentation.h"
#include "../ViewIds.h"
#include "../source/TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <boost/container/small_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  struct TrackListProjectionOperationCounts final
  {
    std::uint64_t fullProjectionRebuilds = 0;
    std::uint64_t incrementalProjectionUpdates = 0;
    std::uint64_t arenaRebases = 0;
    std::uint64_t rowIndexRebuilds = 0;

    bool operator==(TrackListProjectionOperationCounts const&) const = default;
  };

  struct TrackRowRange final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };

  enum class MissingTrackValueKind : std::uint8_t
  {
    Artist,
    Album,
    Year,
    Genre,
    Composer,
    Conductor,
    Ensemble,
    Work,
  };

  using TrackGroupHeadingValue = std::variant<std::monostate, std::string, std::uint16_t, MissingTrackValueKind>;

  struct TrackGroupHeading final
  {
    TrackGroupHeadingValue primary{};
    TrackGroupHeadingValue secondary{};
    TrackGroupHeadingValue tertiary{};

    bool operator==(TrackGroupHeading const&) const = default;
  };

  struct TrackGroupSectionSnapshot final
  {
    TrackRowRange rows{};
    TrackGroupHeading heading{};
    ResourceId imageId{kInvalidResourceId};
  };

  struct ProjectionReset final
  {};

  struct ProjectionInsertRange final
  {
    TrackRowRange range{};
  };

  struct ProjectionRemoveRange final
  {
    TrackRowRange range{};
  };

  struct ProjectionUpdateRange final
  {
    TrackRowRange range{};
  };

  struct ProjectionSourceInvalidated final
  {};

  using TrackListProjectionDelta = std::variant<ProjectionReset,
                                                ProjectionInsertRange,
                                                ProjectionRemoveRange,
                                                ProjectionUpdateRange,
                                                ProjectionSourceInvalidated>;

  struct TrackListProjectionDeltaBatch final
  {
    // Nearly every publish carries exactly one delta, so a small_vector with inline
    // capacity for one element keeps the common case allocation-free. Larger batches
    // (rare) spill to the heap transparently.
    boost::container::small_vector<TrackListProjectionDelta, 1> deltas{};
  };

  /**
   * Validates sequential projection coordinates against an initial row count.
   *
   * Reset and source invalidation are valid only as singleton batches. Every
   * regular range is interpreted after the ranges that precede it.
   */
  bool validateTrackListProjectionDeltaBatch(TrackListProjectionDeltaBatch const& batch, std::size_t initialSize);

  class TrackListProjection final
  {
  public:
    TrackListProjection(ViewId viewId, TrackSourceLease sourceLease, library::MusicLibrary const& library);
    TrackListProjection(ViewId viewId,
                        TrackSourceLease sourceLease,
                        library::MusicLibrary const& library,
                        TrackOrderSpec const& order);
    ~TrackListProjection();

    TrackListProjection(TrackListProjection const&) = delete;
    TrackListProjection& operator=(TrackListProjection const&) = delete;
    TrackListProjection(TrackListProjection&&) = delete;
    TrackListProjection& operator=(TrackListProjection&&) = delete;

    ViewId viewId() const noexcept;

    TrackPresentationSpec presentation() const;
    std::size_t groupCount() const noexcept;
    TrackGroupSectionSnapshot groupAt(std::size_t groupIndex) const;
    std::optional<std::size_t> groupIndexAt(std::size_t rowIndex) const;
    std::optional<TrackRowRange> groupRangeAt(std::size_t rowIndex) const noexcept;

    std::size_t size() const noexcept;
    TrackId trackIdAt(std::size_t index) const;
    std::optional<std::size_t> indexOf(TrackId trackId) const noexcept;

    TrackListProjectionOperationCounts operationCounts() const noexcept;
    void setPresentation(TrackPresentationSpec const& presentation);

    async::Subscription subscribe(std::move_only_function<void(TrackListProjectionDeltaBatch const&) noexcept> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
