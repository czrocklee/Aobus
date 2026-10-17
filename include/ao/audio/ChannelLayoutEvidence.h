// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  enum class ChannelLayoutOrigin : std::uint8_t
  {
    Unknown,
    Identified,
    Inferred,
  };

  // Origin records how the effective speakerMask interpretation was
  // established: read from declared or format-default channel metadata, or
  // inferred from weaker evidence. Raw declaration vocabulary and conflict
  // context stay with the decoder. No origin value alone admits a layout or
  // proves its delivery.
  struct ChannelLayoutEvidence final
  {
    ChannelLayoutOrigin origin = ChannelLayoutOrigin::Unknown;

    bool operator==(ChannelLayoutEvidence const&) const = default;
  };
} // namespace ao::audio
