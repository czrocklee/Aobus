// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/SignalFormat.h>

#include <bit>
#include <cstdint>

namespace ao::audio::backend::detail
{
  // Source admission only: zero remains unidentified; otherwise every channel
  // must have one role in the currently supported WAVE speaker vocabulary.
  // Native observations may retain partial sets and use a different contract.
  constexpr bool hasSupportedSourceSpeakerSet(SignalFormat const& source) noexcept
  {
    constexpr std::uint32_t kSupportedRoles = 0x3FFFFU;
    return source.speakerMask == 0U ||
           ((source.speakerMask & ~kSupportedRoles) == 0U && std::popcount(source.speakerMask) == source.channels);
  }
} // namespace ao::audio::backend::detail
