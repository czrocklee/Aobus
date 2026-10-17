// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  // Facts at the backend's configured software boundary, not physical wiring.
  // Legacy uncertainty and an independently known conversion can coexist.
  struct ChannelDeliveryEvidence final
  {
    // True only when native position/configuration evidence established the
    // mapping. Retaining the application request never sets it.
    bool isMapped = false;
    // Position set established at this native software boundary. Unlike a
    // PcmFormat mask, this does not assert canonical native byte order, and it
    // requires successful native observation or configuration. Zero retains
    // genuinely unknown positions.
    std::uint32_t nativeSpeakerMask = 0;
    // Known channel mixing, dropping or relabelling received at this native
    // boundary. Providers attribute a conversion once, not to every node that
    // repeats its output evidence; independent later conversions remain distinct.
    // Source, Decoder and Engine nodes cannot supply this backend-only fact.
    // Sample-rate, sample-domain and representation changes are independent
    // format evidence, not this flag.
    bool hasSharedConversion = false;

    bool operator==(ChannelDeliveryEvidence const&) const = default;
  };

  // Identity/proof quality of the selected channel path, independent of the
  // numerical quality axes and of alteration findings. Verified describes the
  // observed software path only, not physical wiring.
  enum class ChannelConfidence : std::uint8_t
  {
    Unknown,
    Unverified,
    Inferred,
    Verified,
  };
} // namespace ao::audio
