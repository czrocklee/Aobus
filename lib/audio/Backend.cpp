// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>

#include "detail/DecoderOutput.h"
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <optional>

namespace ao::audio
{
  std::optional<PcmFormat> Backend::prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept
  {
    auto const optEncoding = detail::preferredLosslessPcmEncoding(sourceFormat);

    if (!optEncoding)
    {
      return std::nullopt;
    }

    return pcmFormat(sourceFormat, *optEncoding);
  }
} // namespace ao::audio
