// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/compat/MoveOnlyFunction.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

namespace ao::library
{
  struct AudioIdentity final
  {
    utility::Hash128 signature{};
    std::uint64_t payloadLength = 0;
  };

  // payloadLength == 0 is the persisted "identity pending" sentinel.
  inline bool hasAudioIdentity(std::uint64_t payloadLength, utility::Hash128 signature) noexcept
  {
    return payloadLength != 0 && signature != utility::Hash128{};
  }

  using AudioIdentityProgressCallback = compat::MoveOnlyFunction<void(double fraction)>;

  /**
   * Compute the identity of the file's encoded audio payload.
   *
   * audioPayload must be non-empty. Cancellation returns an empty optional. A
   * completed calculation returns an identity. File opening and encoded-payload
   * parsing belong to the caller and retain their own Result channels.
   */
  std::optional<AudioIdentity> readAudioIdentity(std::span<std::byte const> audioPayload,
                                                 AudioIdentityProgressCallback progress = {},
                                                 std::stop_token stopToken = {});
} // namespace ao::library
