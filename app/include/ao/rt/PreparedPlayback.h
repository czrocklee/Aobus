// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>

namespace ao::rt
{
  /** Opaque transport-owner-lifetime identity for one prepared-next request. */
  struct PreparedNextToken final
  {
    std::uint64_t value = 0;

    bool operator==(PreparedNextToken const&) const = default;
  };

  /**
   * Runtime wrapper for audio's callback-generation cancellation proof.
   * It covers only commitments issued in strictly older generations.
   */
  struct PreparedCancellationBarrier final
  {
    std::uint64_t generation = 0;

    bool covers(std::uint64_t issuedGeneration) const noexcept { return issuedGeneration < generation; }
    bool operator==(PreparedCancellationBarrier const&) const = default;
  };

  struct PlaybackStartReceipt final
  {
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    PreparedCancellationBarrier cancellationBarrier{};

    bool operator==(PlaybackStartReceipt const&) const = default;
  };
} // namespace ao::rt
