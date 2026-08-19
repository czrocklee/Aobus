// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/media/ogg/Demuxer.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ao::media::opus
{
  struct Head;

  /**
   * @brief Number of 48 kHz samples one Opus packet decodes to, read from its
   *        table-of-contents byte.
   *
   * Returns nullopt for a packet that declares no frames, declares more than
   * the 120 ms an Opus packet may hold, or is too short to carry its own frame
   * count. Only the first one or two bytes are examined, so this stays usable
   * on a reader that never links libopus.
   */
  std::optional<std::int32_t> packetSampleCount(std::span<std::byte const> packet) noexcept;

  /**
   * @brief Where an Ogg Opus stream starts and how much audio it holds.
   *
   * RFC 7845 section 4.1 lets the first audio page declare a granule position
   * larger than the audio it carries, which crops the front of a stream or
   * joins a live one without renumbering every later page. The origin that
   * establishes is the single quantity every position in the stream is measured
   * against, so duration, trimming, and seeking all derive from here rather
   * than each recomputing it.
   */
  struct Timeline final
  {
    // Granule position decoding begins from, before the pre-skip is discarded.
    // Zero for a stream that was not cropped.
    std::int64_t decodeStartGranule = 0;
    // Granule position of the first audible sample, which is decodeStartGranule
    // advanced past the pre-skip. Pre-skip is a length measured from the decode
    // start, not a region of the absolute timeline, so the two add.
    std::int64_t playbackStartGranule = 0;
    // Audible frames the stream holds. Engaged and zero is a stream that is
    // valid and silent; disengaged means no complete ending established a
    // length, and a reader must decode what it finds.
    std::optional<std::uint64_t> optTotalFrames;
  };

  /**
   * @brief Derives the timeline of the Opus stream carried by an Ogg demuxer.
   *
   * @return Error::Code::CorruptData when the stream carries no audio, holds a
   *         packet whose table of contents is unusable, gives a non-final first
   *         audio page fewer granules than its completed packets decode, or
   *         completes before its own pre-skip.
   */
  Result<Timeline> deriveOggTimeline(ogg::Demuxer const& demuxer, Head const& head);
} // namespace ao::media::opus
