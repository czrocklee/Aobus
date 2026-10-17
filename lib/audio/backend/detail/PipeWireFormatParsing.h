// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

extern "C"
{
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace ao::audio::backend::detail
{
  struct RawStreamFormat final
  {
    PcmFormat format{};
    // Set identity survives noncanonical native slot order. In that case the
    // PcmFormat mask stays zero rather than asserting canonical byte order.
    std::uint32_t nativeSpeakerMask = 0;
    bool hasUnsupportedPositions = false;
    std::uint32_t flags = 0;
    std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS> positions{};

    bool operator==(RawStreamFormat const&) const = default;
  };

  struct RawStreamDelivery final
  {
    PcmFormat clientFormat{};
    ChannelDeliveryEvidence channelDelivery{.isMapped = false};
    // Native destination slot -> canonical application source slot.
    std::array<std::uint8_t, SPA_AUDIO_MAX_CHANNELS> sourceSlots{};
    bool needsPermutation = false;
  };

  bool acceptsRawStreamLayout(SignalFormat const& sourceFormat) noexcept;

  /**
   * @brief Builds an ordered raw-audio offer in caller-owned storage.
   *
   * SPA enum choices store a default before their alternatives. The preferred
   * encoding is emitted twice: as the default and first valid alternative.
   * Only the two-channel cohort offers explicit canonical positions.
   */
  ::spa_pod const* buildRawStreamFormatOffer(std::span<std::byte> buffer,
                                             SignalFormat const& sourceFormat,
                                             std::span<SampleEncoding const> encodings) noexcept;

  // Bounded client readback: malformed positioning/storage fails; unsupported
  // roles remain observations for contextual admission, not guessed identities.
  std::optional<RawStreamFormat> parseRawStreamObservation(::spa_pod const* param) noexcept;
  // Best-effort current monitor facts: unusable roles do not discard valid
  // numerics or known role subsets. This is not client-admission evidence.
  std::optional<RawStreamFormat> parseRawMonitorObservation(::spa_pod const* param) noexcept;

  // Validates the actual client readback, never an endpoint or request copy.
  std::expected<RawStreamDelivery, BackendOpenFailure> rawStreamDelivery(
    SignalFormat const& sourceFormat,
    RawStreamFormat const& native,
    std::optional<RawStreamFormat> const& optPrevious = std::nullopt);
  void applyRawStreamDelivery(std::span<std::byte> bytes, RawStreamDelivery const& delivery) noexcept;
} // namespace ao::audio::backend::detail
