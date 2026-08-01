// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

namespace ao::audio
{
  struct OpenedPcmMode;
  struct SignalFormat;
}

namespace ao::audio::detail
{
  /**
   * @brief Rejects an open result that playback must not accept.
   *
   * Sample rate and channel count must survive untouched, because no stage in
   * the graph resamples or remixes. The client encoding and every confirmed
   * endpoint must preserve the source precision. Endpoint evidence describes
   * the physical route and never turns a lossy mode into an admissible one.
   */
  Result<> validateOpenedMode(SignalFormat const& sourceFormat, OpenedPcmMode const& mode);
} // namespace ao::audio::detail
