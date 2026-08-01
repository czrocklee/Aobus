// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

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
   * the physical route but never authorizes a precision reduction.
   */
  struct ConfirmedEndpoint final
  {
    SignalFormat signalFormat{};

    bool operator==(ConfirmedEndpoint const&) const = default;
  };

  /**
   * @brief Outcome of one successful Backend::open.
   *
   * `clientFormat` is the byte layout Aobus hands to the native API.
   * `optEndpoint` is the confirmed endpoint behind it. An absent endpoint means
   * the backend could not confirm the direct endpoint during this open; it does
   * not mean the endpoint equals the client format. Both the client mode and
   * every confirmed endpoint must preserve the source signal precision.
   */
  struct OpenedPcmMode final
  {
    PcmFormat clientFormat{};
    std::optional<ConfirmedEndpoint> optEndpoint{};

    bool operator==(OpenedPcmMode const&) const = default;
  };
} // namespace ao::audio
