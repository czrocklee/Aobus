// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <optional>
#include <vector>

namespace ao::audio::detail
{
  /** Ordered PCM encodings that preserve every bit of the inspected signal. */
  std::vector<SampleEncoding> losslessPcmEncodings(SignalFormat const& sourceFormat);

  std::optional<SampleEncoding> preferredLosslessPcmEncoding(SignalFormat const& sourceFormat) noexcept;

  bool isLosslessPcmEncoding(SignalFormat const& sourceFormat, SampleEncoding encoding) noexcept;
} // namespace ao::audio::detail
