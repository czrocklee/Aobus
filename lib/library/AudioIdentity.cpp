// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/AudioIdentity.h>
#include <ao/utility/Xxh3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kAudioIdentityChunkSize = 4ULL * 1024ULL * 1024ULL;
  }

  std::optional<AudioIdentity> readAudioIdentity(std::span<std::byte const> audioPayload,
                                                 AudioIdentityProgressCallback progress,
                                                 std::stop_token stopToken)
  {
    auto accumulator = utility::Xxh3Accumulator128{};
    std::size_t processed = 0;

    if (progress)
    {
      progress(0.0);
    }

    while (processed < audioPayload.size())
    {
      if (stopToken.stop_requested())
      {
        return {};
      }

      auto const remaining = audioPayload.size() - processed;
      auto const chunkSize = std::min(kAudioIdentityChunkSize, remaining);
      accumulator.mix(audioPayload.subspan(processed, chunkSize));
      processed += chunkSize;

      if (progress)
      {
        auto const fraction =
          audioPayload.empty() ? 1.0 : static_cast<double>(processed) / static_cast<double>(audioPayload.size());
        progress(fraction);
      }

      if (stopToken.stop_requested())
      {
        return {};
      }
    }

    if (audioPayload.empty() && progress)
    {
      progress(1.0);
    }

    return AudioIdentity{
      .signature = accumulator.value(),
      .payloadLength = static_cast<std::uint64_t>(audioPayload.size()),
    };
  }
} // namespace ao::library
