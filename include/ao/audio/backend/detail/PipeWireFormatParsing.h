// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

extern "C"
{
#include <spa/pod/pod.h>
}

#include <cstddef>
#include <optional>
#include <span>

namespace ao::audio::backend::detail
{
  /**
   * @brief Builds an ordered raw-audio offer in caller-owned storage.
   *
   * SPA enum choices store a default before their alternatives. The preferred
   * encoding is therefore emitted twice: once as the default and once as the
   * first valid alternative.
   */
  ::spa_pod const* buildRawStreamFormatOffer(std::span<std::byte> buffer,
                                             SignalFormat const& sourceFormat,
                                             std::span<SampleEncoding const> encodings) noexcept;

  std::optional<PcmFormat> parseRawStreamFormat(::spa_pod const* param) noexcept;
} // namespace ao::audio::backend::detail
