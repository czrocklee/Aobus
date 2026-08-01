// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

namespace ao::audio
{
  /**
   * @brief Snapshot of the active playback route's format chain.
   *
   * Lives in a public audio header so Engine/Player status structs can embed it
   * without exposing RouteTracker internals.
   */
  struct AudioRouteFormatState final
  {
    SignalFormat sourceFormat;
    PcmFormat decoderOutputFormat;
    PcmFormat engineOutputFormat;
    bool isLossySource = false;
    AudioCodec codec = AudioCodec::Unknown;

    bool operator==(AudioRouteFormatState const&) const = default;
  };
} // namespace ao::audio
