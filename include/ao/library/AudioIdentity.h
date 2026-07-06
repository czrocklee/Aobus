// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/utility/Fnv1a.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

namespace ao::tag
{
  class TagFile;
}

namespace ao::library
{
  struct AudioIdentity final
  {
    utility::Hash128 signature{};
    std::uint64_t payloadLength = 0;
  };

  // payloadLength == 0 is the "identity pending" sentinel: real encoded audio
  // payloads are never empty. A parser reporting an empty payload for a
  // decodable file is a corrupt-file fault, not a pending identity.
  inline bool hasAudioIdentity(std::uint64_t payloadLength, utility::Hash128 signature) noexcept
  {
    return payloadLength != 0 && signature != utility::Hash128{};
  }

  using AudioIdentityProgressCallback = std::move_only_function<void(double fraction)>;

  /**
   * Compute the identity of the file's encoded audio payload.
   *
   * Cancellation is not an error: when @p stopToken is stop-requested the
   * function returns an engaged Result holding an empty optional (per the
   * error model, cooperative cancellation is not carried as an Error). The
   * error channel is used only for I/O and parse failures; a successful,
   * uncancelled run always returns an engaged optional.
   */
  Result<std::optional<AudioIdentity>> readAudioIdentity(tag::TagFile const& tagFile,
                                                         AudioIdentityProgressCallback progress = {},
                                                         std::stop_token stopToken = {});

  Result<std::optional<AudioIdentity>> readAudioIdentity(std::filesystem::path const& path,
                                                         AudioIdentityProgressCallback progress = {},
                                                         std::stop_token stopToken = {});
} // namespace ao::library
