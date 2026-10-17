// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <optional>

namespace ao::audio
{
  /**
   * @brief Endpoint signal confirmed by one native open and configuration.
   *
   * A backend supplies it only after the native API reported the endpoint that
   * the configured client format actually feeds; it is never predicted, cached
   * across opens, or copied from the client format. Endpoint evidence describes
   * the physical route but never authorizes a precision reduction. A zero
   * speaker mask leaves positions unconfirmed even when precision is known.
   */
  struct ConfirmedEndpoint final
  {
    SignalFormat signalFormat{};

    bool operator==(ConfirmedEndpoint const&) const = default;
  };

  /**
   * @brief Outcome of one successful Backend::open.
   *
   * `clientFormat` is the canonical PCM pulled from the render target. Native
   * descriptors and any lossless slot permutation remain backend-owned.
   * `optEndpoint` is the confirmed endpoint behind it. An absent endpoint means
   * the backend could not confirm the direct endpoint during this open; it does
   * not mean the endpoint equals the client format. Both the client mode and
   * every confirmed endpoint must preserve the source signal precision.
   * The client mask must retain the application identity; an unknown native
   * mask is not made known by copying it from the source request.
   */
  struct OpenedPcmMode final
  {
    PcmFormat clientFormat{};
    std::optional<ConfirmedEndpoint> optEndpoint{};
    // Initial configured-boundary snapshot. Current provider observations
    // supersede it; neither value is evidence of physical speaker wiring.
    ChannelDeliveryEvidence channelDelivery{};

    bool operator==(OpenedPcmMode const&) const = default;
  };
} // namespace ao::audio
