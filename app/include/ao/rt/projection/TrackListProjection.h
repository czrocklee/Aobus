// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../Subscription.h"
#include "../TrackPresentation.h"
#include "../ViewIds.h"
#include <ao/CoreIds.h>

#include <boost/container/small_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace ao::rt
{
  struct TrackRowRange final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };

  struct TrackGroupSectionSnapshot final
  {
    TrackRowRange rows{};
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
    std::uint64_t revision = 0;

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
  inline bool validateTrackListProjectionDeltaBatch(TrackListProjectionDeltaBatch const& batch,
                                                    std::size_t initialSize) noexcept
  {
    if (batch.deltas.empty())
    {
      return false;
    }

    if (std::holds_alternative<ProjectionReset>(batch.deltas.front()) ||
        std::holds_alternative<ProjectionSourceInvalidated>(batch.deltas.front()))
    {
      return batch.deltas.size() == 1;
    }

    auto size = initialSize;

    for (auto const& delta : batch.deltas)
    {
      if (auto const* insertion = std::get_if<ProjectionInsertRange>(&delta); insertion != nullptr)
      {
        if (insertion->range.count == 0 || insertion->range.start > size ||
            insertion->range.count > std::numeric_limits<std::size_t>::max() - size)
        {
          return false;
        }

        size += insertion->range.count;
        continue;
      }

      if (auto const* removal = std::get_if<ProjectionRemoveRange>(&delta); removal != nullptr)
      {
        if (removal->range.count == 0 || removal->range.start > size ||
            removal->range.count > size - removal->range.start)
        {
          return false;
        }

        size -= removal->range.count;
        continue;
      }

      if (auto const* update = std::get_if<ProjectionUpdateRange>(&delta);
          update == nullptr || update->range.count == 0 || update->range.start > size ||
          update->range.count > size - update->range.start)
      {
        return false;
      }
    }

    return true;
  }

  class TrackListProjection
  {
  public:
    virtual ~TrackListProjection() = default;

    TrackListProjection(TrackListProjection const&) = delete;
    TrackListProjection& operator=(TrackListProjection const&) = delete;
    TrackListProjection(TrackListProjection&&) = delete;
    TrackListProjection& operator=(TrackListProjection&&) = delete;

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
    TrackListProjection() = default;
  };
} // namespace ao::rt
