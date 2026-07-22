// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::rt
{
  /** Opaque transport-owner-lifetime identity for one prepared-next request. */
  struct PreparedNextToken final
  {
    std::uint64_t value = 0;
    std::uint64_t issuedGeneration = 0;

    bool operator==(PreparedNextToken const&) const = default;
  };

  /**
   * Runtime wrapper for audio's callback-generation cancellation proof.
   * It covers only commitments issued in strictly older generations.
   */
  struct PreparedCancellationBarrier final
  {
    std::uint64_t generation = 0;

    bool covers(PreparedNextToken const token) const noexcept { return token.issuedGeneration < generation; }
    bool operator==(PreparedCancellationBarrier const&) const = default;
  };
} // namespace ao::rt
