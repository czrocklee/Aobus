// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/SignalFormat.h>

namespace ao::audio::backend::detail
{
  // Source admission only: zero remains unidentified; otherwise every channel
  // must have one role in the currently supported WAVE speaker vocabulary.
  // Native observations may retain partial sets and use a different contract.
  constexpr bool hasSupportedSourceSpeakerSet(SignalFormat const& source) noexcept
  {
    return source.speakerMask == 0U || isCompleteWaveSpeakerMask(source.speakerMask, source.channels);
  }
} // namespace ao::audio::backend::detail
