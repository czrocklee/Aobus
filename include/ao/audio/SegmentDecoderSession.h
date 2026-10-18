// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PlaybackInput.h>

#include <cstdint>
#include <memory>

namespace ao::audio
{
  // Validates the stored carrier facts and positions the decoder at the exact
  // segment start before exposing any PCM. All public coordinates are relative.
  Result<std::unique_ptr<DecoderSession>> makeSegmentDecoderSession(std::unique_ptr<DecoderSession> decoderPtr,
                                                                    PlaybackSegment const& segment,
                                                                    std::uint32_t expectedRate) noexcept;
} // namespace ao::audio
