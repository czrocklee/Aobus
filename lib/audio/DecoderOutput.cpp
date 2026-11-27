// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/DecoderOutput.h"

#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <array>
#include <optional>
#include <vector>

namespace ao::audio::detail
{
  namespace
  {
    constexpr auto kPreferredOrder = std::to_array<SampleEncoding>({SampleEncoding::Signed16Le,
                                                                    SampleEncoding::Signed24PackedLe,
                                                                    SampleEncoding::Signed24In32Le,
                                                                    SampleEncoding::Signed32Le,
                                                                    SampleEncoding::Float32Le});
  } // namespace

  bool isLosslessPcmEncoding(SignalFormat const& sourceFormat, SampleEncoding const encoding) noexcept
  {
    if (sourceFormat.sampleKind == SampleKind::FloatingPoint)
    {
      return sourceFormat.precisionBits == 32U && encoding == SampleEncoding::Float32Le;
    }

    if (sourceFormat.precisionBits == 0U || sourceFormat.precisionBits > 32U)
    {
      return false;
    }

    // Float32 has 24 bits of significand precision for integer PCM, not 32.
    auto const availableBits = isFloatEncoding(encoding) ? 24U : encodingNominalBits(encoding);
    return availableBits >= sourceFormat.precisionBits;
  }

  std::vector<SampleEncoding> losslessPcmEncodings(SignalFormat const& sourceFormat)
  {
    auto encodings = std::vector<SampleEncoding>{};

    for (auto const encoding : kPreferredOrder)
    {
      if (isLosslessPcmEncoding(sourceFormat, encoding))
      {
        encodings.push_back(encoding);
      }
    }

    return encodings;
  }

  std::optional<SampleEncoding> preferredLosslessPcmEncoding(SignalFormat const& sourceFormat) noexcept
  {
    for (auto const encoding : kPreferredOrder)
    {
      if (isLosslessPcmEncoding(sourceFormat, encoding))
      {
        return encoding;
      }
    }

    return std::nullopt;
  }
} // namespace ao::audio::detail
