// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstdint>
#include <optional>
#include <span>

namespace ao::audio::backend::detail
{
  /**
   * @brief One encoding the opened handle still admits, with its endpoint width.
   *
   * This is a snapshot taken inside a single open() after access, rate, and
   * channels are fixed. It is not a device capability record: it never outlives
   * the open that produced it and is never consulted to predict a later open.
   */
  struct AlsaModeEvidence final
  {
    SampleEncoding encoding = SampleEncoding::Unknown;
    std::optional<std::uint8_t> optSignificantBits{};

    bool operator==(AlsaModeEvidence const&) const = default;
  };

  /** @brief Mode chosen from evidence, with the endpoint precision behind it. */
  struct SelectedAlsaMode final
  {
    SampleEncoding encoding = SampleEncoding::Unknown;
    std::uint8_t endpointPrecisionBits = 0;

    bool operator==(SelectedAlsaMode const&) const = default;
  };

  /**
   * @brief Picks the first confirmed lossless encoding for one source signal.
   *
   * Encodings are considered in the documented lossless order. A candidate is
   * admissible only when the opened handle confirms enough significant bits to
   * preserve the source. Missing evidence and lower-precision endpoints are
   * rejected rather than interpreted as permission to reduce precision.
   */
  Result<SelectedAlsaMode> selectAlsaMode(SignalFormat const& sourceFormat, std::span<AlsaModeEvidence const> evidence);
} // namespace ao::audio::backend::detail
