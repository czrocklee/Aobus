// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaModeSelector.h"

#include "../../detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <algorithm>
#include <format>
#include <span>
#include <string>

namespace ao::audio::backend::detail
{
  namespace
  {
    std::string describeEvidence(std::span<AlsaModeEvidence const> evidence)
    {
      if (evidence.empty())
      {
        return "none";
      }

      auto text = std::string{};

      for (auto const& entry : evidence)
      {
        if (!text.empty())
        {
          text += ", ";
        }

        text += entry.optSignificantBits
                  ? std::format("{} sbits={}", sampleEncodingName(entry.encoding), *entry.optSignificantBits)
                  : std::format("{} sbits=unknown", sampleEncodingName(entry.encoding));
      }

      return text;
    }
  } // namespace

  Result<SelectedAlsaMode> selectAlsaMode(SignalFormat const& sourceFormat, std::span<AlsaModeEvidence const> evidence)
  {
    if (sourceFormat.precisionBits == 0U)
    {
      return makeError(Error::Code::InvalidInput, "Source signal declares no precision");
    }

    auto const findEvidence = [evidence](SampleEncoding const encoding) -> AlsaModeEvidence const*
    {
      auto const it = std::ranges::find(evidence, encoding, &AlsaModeEvidence::encoding);
      return it == evidence.end() ? nullptr : &*it;
    };

    // A lossless encoding still needs an endpoint wide enough to carry the
    // signal: S32_LE driving a 24-bit converter is not lossless for 32-bit
    // content, and only the significant-bit count can tell the two apart.
    for (auto const encoding : ao::audio::detail::losslessPcmEncodings(sourceFormat))
    {
      auto const* const entry = findEvidence(encoding);

      if (entry == nullptr)
      {
        continue;
      }

      if (!entry->optSignificantBits)
      {
        continue;
      }

      auto const endpointBits = std::min(encodingNominalBits(entry->encoding), *entry->optSignificantBits);

      if (endpointBits >= sourceFormat.precisionBits)
      {
        return SelectedAlsaMode{.encoding = encoding, .endpointPrecisionBits = endpointBits};
      }
    }

    auto const* const domain = sourceFormat.sampleKind == SampleKind::FloatingPoint ? "float" : "integer";
    return makeError(Error::Code::FormatRejected,
                     std::format("ALSA offers no confirmed lossless endpoint for a {}-bit {} signal; evidence: {}",
                                 sourceFormat.precisionBits,
                                 domain,
                                 describeEvidence(evidence)));
  }
} // namespace ao::audio::backend::detail
