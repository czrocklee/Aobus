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
  class LibraryMutationService;

  enum class LibraryAuthoringState : std::uint8_t
  {
    Available,
    Maintenance,
    Faulted,
  };

  enum class LibraryMaintenanceKind : std::uint8_t
  {
    None,
    Import,
    ScanApply,
    AudioIdentityBackfill,
  };

  struct LibraryAuthoringAvailability final
  {
    LibraryAuthoringState state = LibraryAuthoringState::Available;
    std::uint64_t runtimeInstanceId = 0;
    std::uint64_t libraryRevision = 0;
    LibraryMaintenanceKind maintenanceKind = LibraryMaintenanceKind::None;

    bool operator==(LibraryAuthoringAvailability const&) const = default;
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

    std::uint64_t runtimeInstanceId() const noexcept { return _runtimeInstanceId; }
    std::uint64_t libraryRevision() const noexcept { return _libraryRevision; }
    std::span<TrackId const> trackIds() const noexcept { return _trackIds; }

    bool operator==(BoundTrackTargets const&) const = default;

  private:
    BoundTrackTargets(std::uint64_t runtimeInstanceId, std::uint64_t libraryRevision, std::vector<TrackId> trackIds)
      : _runtimeInstanceId{runtimeInstanceId}, _libraryRevision{libraryRevision}, _trackIds{std::move(trackIds)}
    {
    }

    std::uint64_t _runtimeInstanceId = 0;
    std::uint64_t _libraryRevision = 0;
    std::vector<TrackId> _trackIds;

    friend class LibraryMutationService;
  };

  enum class TrackAuthoringStatus : std::uint8_t
  {
    Applied,
    NoOp,
    Stale,
    Missing,
    Unavailable,
  };

  template<typename Reply>
  struct TrackAuthoringResult final
  {
    TrackAuthoringStatus status = TrackAuthoringStatus::NoOp;
    Reply reply{};
    std::optional<BoundTrackTargets> optNextTargets{};
  };
} // namespace ao::rt
