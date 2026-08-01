// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace ao::audio::backend::detail
{
  /**
   * @brief Bounded record of client formats a device actually accepted.
   *
   * Entries are keyed on the complete signal, never generalized across sample
   * rate or channel count: ALSA admits formats as a joint constraint, so a
   * device may offer 24-bit at 48 kHz and only 16-bit at 96 kHz. Several
   * entries are kept so a library alternating between common signals does not
   * evict itself on every track.
   *
   * This is advisory only. It feeds prewarm prediction, never correctness: a
   * stale entry costs one discarded optimistic decoder. Only successful
   * lossless modes are stored. The cache is owned by one backend instance and
   * therefore starts empty whenever the selected device changes.
   */
  class AlsaPrewarmCache final
  {
  public:
    static constexpr std::size_t kCapacity = 8;

    std::optional<PcmFormat> find(SignalFormat const& sourceFormat) const noexcept;

    void store(SignalFormat const& sourceFormat, PcmFormat const& clientFormat);

    void clear() noexcept { _entries.clear(); }

  private:
    struct Entry final
    {
      SignalFormat sourceFormat{};
      PcmFormat clientFormat{};
    };

    std::vector<Entry> _entries;
  };
} // namespace ao::audio::backend::detail
