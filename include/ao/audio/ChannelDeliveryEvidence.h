// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  // Facts at the backend's configured software boundary, not physical wiring.
  // A mapped relationship requires native position/configuration evidence;
  // merely retaining the application mask does not establish it. Legacy
  // uncertainty and an independently known conversion can coexist.
  struct ChannelDeliveryEvidence final
  {
    enum class Mapping : std::uint8_t
    {
      Unknown,
      LegacyUnverified,
      Mapped,
    };

    Mapping mapping = Mapping::Unknown;
    // Position set established at this native software boundary. Unlike a
    // PcmFormat mask, this does not assert canonical native byte ordering.
    // It requires successful native configuration or observation, never just
    // the application request. Zero retains genuinely unknown positions.
    std::uint32_t nativeSpeakerMask = 0;
    // Known channel mixing, dropping or relabelling. Sample-rate, sample-domain
    // and representation changes are independent format evidence, not this flag.
    bool hasSharedConversion = false;

    bool operator==(ChannelDeliveryEvidence const&) const = default;
  };

  // Independent of format-presence verification and the numerical quality axes.
  // Verified describes the observed software path only, not physical wiring.
  enum class ChannelConfidence : std::uint8_t
  {
    Unknown,
    Unverified,
    Inferred,
    Verified,
    Converted,
  };
} // namespace ao::audio
