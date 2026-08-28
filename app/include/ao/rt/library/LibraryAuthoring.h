// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  class LibraryWriteLane;

  enum class LibraryAuthoringState : std::uint8_t
  {
    Available,
    Maintenance,
  };

  struct LibraryAuthoringAvailability final
  {
    LibraryAuthoringState state = LibraryAuthoringState::Available;
    std::uint64_t runtimeInstanceId = 0;
    std::uint64_t libraryRevision = 0;

    bool operator==(LibraryAuthoringAvailability const&) const = default;
  };

  struct LibraryStamp final
  {
    std::uint64_t runtimeId = 0;
    std::uint64_t revision = 0;

    bool matches(LibraryAuthoringAvailability const& availability) const noexcept
    {
      return availability.state == LibraryAuthoringState::Available && availability.runtimeInstanceId == runtimeId &&
             availability.libraryRevision == revision;
    }

    bool operator==(LibraryStamp const&) const = default;
  };

  /**
   * Immutable evidence that a runtime observed an exact set of tracks at one
   * committed library revision. Construction remains runtime-owned; mutation
   * revalidates every field before using the evidence.
   */
  class BoundTrackTargets final
  {
  public:
    BoundTrackTargets(BoundTrackTargets const&) = default;
    BoundTrackTargets& operator=(BoundTrackTargets const&) = default;
    BoundTrackTargets(BoundTrackTargets&&) noexcept = default;
    BoundTrackTargets& operator=(BoundTrackTargets&&) noexcept = default;
    ~BoundTrackTargets() = default;

    bool matches(LibraryAuthoringAvailability const& availability) const noexcept
    {
      return _stamp.matches(availability);
    }

    std::span<TrackId const> trackIds() const noexcept { return _trackIds; }

    bool operator==(BoundTrackTargets const&) const = default;

  private:
    BoundTrackTargets(std::uint64_t runtimeInstanceId, std::uint64_t libraryRevision, std::vector<TrackId> trackIds)
      : _stamp{.runtimeId = runtimeInstanceId, .revision = libraryRevision}, _trackIds{std::move(trackIds)}
    {
    }

    LibraryStamp _stamp{};
    std::vector<TrackId> _trackIds;

    friend class LibraryWriteLane;
  };

  /**
   * Immutable evidence for one saved List's complete effective source order at
   * a committed library revision.
   */
  class BoundListOrder final
  {
  public:
    BoundListOrder(BoundListOrder const&) = default;
    BoundListOrder& operator=(BoundListOrder const&) = default;
    BoundListOrder(BoundListOrder&&) noexcept = default;
    BoundListOrder& operator=(BoundListOrder&&) noexcept = default;
    ~BoundListOrder() = default;

    bool matches(LibraryAuthoringAvailability const& availability) const noexcept
    {
      return _stamp.matches(availability);
    }

    ListId listId() const noexcept { return _listId; }
    std::span<TrackId const> effectiveTrackIds() const noexcept { return _effectiveTrackIds; }

    bool operator==(BoundListOrder const&) const = default;

  private:
    BoundListOrder(std::uint64_t runtimeInstanceId,
                   std::uint64_t libraryRevision,
                   ListId listId,
                   std::vector<TrackId> effectiveTrackIds)
      : _stamp{.runtimeId = runtimeInstanceId, .revision = libraryRevision}
      , _listId{listId}
      , _effectiveTrackIds{std::move(effectiveTrackIds)}
    {
    }

    LibraryStamp _stamp{};
    ListId _listId = kInvalidListId;
    std::vector<TrackId> _effectiveTrackIds;

    friend class LibraryWriteLane;
  };

  enum class AuthoringStatus : std::uint8_t
  {
    Applied,
    NoOp,
    Busy,
    Stale,
    Unavailable,
  };

  template<typename Reply>
  struct AuthoringResult final
  {
    AuthoringStatus status = AuthoringStatus::NoOp;
    Reply reply{};
  };

  template<typename Reply>
  struct TrackAuthoringResult final
  {
    AuthoringStatus status = AuthoringStatus::NoOp;
    Reply reply{};
    std::optional<BoundTrackTargets> optNextTargets{};
  };
} // namespace ao::rt
