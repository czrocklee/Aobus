// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

namespace ao::audio::detail
{
  // A candidate only, never a wildcard or a transport proof. Each backend must
  // independently establish that both identities use its installed byte order
  // and unchanged admission/refresh obligations before permitting reuse.
  constexpr bool isCanonicalStereoReuseCandidate(PcmFormat const& configured,
                                                 PcmFormat const& current,
                                                 PcmFormat const& next) noexcept
  {
    return configured.sampleRate != 0U && configured.encoding != SampleEncoding::Unknown && configured.channels == 2U &&
           current.channels == 2U && next.channels == 2U && configured.sampleRate == current.sampleRate &&
           current.sampleRate == next.sampleRate && configured.encoding == current.encoding &&
           current.encoding == next.encoding && (configured.speakerMask == 0U || configured.speakerMask == 0x3U) &&
           ((current.speakerMask == 0U && next.speakerMask == 0x3U) ||
            (current.speakerMask == 0x3U && next.speakerMask == 0U));
  }
} // namespace ao::audio::detail
