// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/DecoderOutput.h"

#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <optional>
#include <vector>

namespace ao::audio::detail
{
  bool isLosslessPcmEncoding(SignalFormat const& sourceFormat, SampleEncoding const encoding) noexcept
  {
    if (sourceFormat.sampleKind == SampleKind::FloatingPoint)
    {
      return sourceFormat.precisionBits == 32U && encoding == SampleEncoding::Float32Le;
    }

    if (sourceFormat.precisionBits == 0U || sourceFormat.precisionBits > 32U || isFloatEncoding(encoding))
    {
      return sourceFormat.precisionBits != 0U && sourceFormat.precisionBits <= 24U &&
             encoding == SampleEncoding::Float32Le;
    }

    return encodingNominalBits(encoding) >= sourceFormat.precisionBits;
  }

  std::vector<SampleEncoding> losslessPcmEncodings(SignalFormat const& sourceFormat)
  {
    auto encodings = std::vector<SampleEncoding>{};

    if (sourceFormat.sampleKind == SampleKind::FloatingPoint)
    {
      if (sourceFormat.precisionBits == 32U)
      {
        encodings.push_back(SampleEncoding::Float32Le);
      }

      return encodings;
    }

    if (sourceFormat.precisionBits == 0U || sourceFormat.precisionBits > 32U)
    {
      return encodings;
    }

    if (sourceFormat.precisionBits <= 16U)
    {
      encodings.push_back(SampleEncoding::Signed16Le);
    }

    if (sourceFormat.precisionBits <= 24U)
    {
      encodings.push_back(SampleEncoding::Signed24PackedLe);
      encodings.push_back(SampleEncoding::Signed24In32Le);
    }

    encodings.push_back(SampleEncoding::Signed32Le);

    if (sourceFormat.precisionBits <= 24U)
    {
      encodings.push_back(SampleEncoding::Float32Le);
    }

    return encodings;
  }

  std::optional<SampleEncoding> preferredLosslessPcmEncoding(SignalFormat const& sourceFormat) noexcept
  {
    if (sourceFormat.sampleKind == SampleKind::FloatingPoint)
    {
      return sourceFormat.precisionBits == 32U ? std::optional{SampleEncoding::Float32Le} : std::nullopt;
    }

    if (sourceFormat.precisionBits == 0U || sourceFormat.precisionBits > 32U)
    {
      return std::nullopt;
    }

    if (sourceFormat.precisionBits <= 16U)
    {
      return SampleEncoding::Signed16Le;
    }

    if (sourceFormat.precisionBits <= 24U)
    {
      return SampleEncoding::Signed24PackedLe;
    }

    return SampleEncoding::Signed32Le;
  }
} // namespace ao::audio::detail
