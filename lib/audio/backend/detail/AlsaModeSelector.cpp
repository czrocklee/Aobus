// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlsaModeSelector.h"

#include "../../detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <algorithm>
#include <expected>
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

  std::expected<SelectedAlsaMode, BackendOpenFailure> selectAlsaMode(SignalFormat const& sourceFormat,
                                                                     std::span<AlsaModeEvidence const> evidence)
  {
    if (sourceFormat.precisionBits == 0U)
    {
      return std::unexpected{
        BackendOpenFailure{.error = makeError(Error::Code::InvalidInput, "Source signal declares no precision").error(),
                           .cause = BackendOpenFailure::Cause::TrackRouteIncompatible}};
    }

    auto const encodings = ao::audio::detail::losslessPcmEncodings(sourceFormat);
    bool hasUnknownPrecision = false;

    auto const findEvidence = [evidence](SampleEncoding const encoding) -> AlsaModeEvidence const*
    {
      auto const it = std::ranges::find(evidence, encoding, &AlsaModeEvidence::encoding);
      return it == evidence.end() ? nullptr : &*it;
    };

    // A lossless encoding still needs an endpoint wide enough to carry the
    // signal: S32_LE driving a 24-bit converter is not lossless for 32-bit
    // content, and only the significant-bit count can tell the two apart.
    for (auto const encoding : encodings)
    {
      auto const* const entry = findEvidence(encoding);

      if (entry == nullptr)
      {
        continue;
      }

      if (!entry->optSignificantBits || *entry->optSignificantBits == 0U)
      {
        hasUnknownPrecision = true;
        continue;
      }

      auto const endpointBits = std::min(encodingNominalBits(entry->encoding), *entry->optSignificantBits);

      if (endpointBits >= sourceFormat.precisionBits)
      {
        return SelectedAlsaMode{.encoding = encoding, .endpointPrecisionBits = endpointBits};
      }
    }

    auto const* const domain = sourceFormat.sampleKind == SampleKind::FloatingPoint ? "float" : "integer";
    // Missing precision for an admitted lossless encoding is not proof of an
    // incompatible endpoint. No admissible encoding, or confirmed narrower
    // modes only, establishes an intentional refusal.
    auto const cause = hasUnknownPrecision ? BackendOpenFailure::Cause::OutputFailure
                                           : BackendOpenFailure::Cause::TrackRouteIncompatible;
    return std::unexpected{BackendOpenFailure{
      .error = makeError(Error::Code::FormatRejected,
                         std::format("ALSA offers no confirmed lossless endpoint for a {}-bit {} signal; evidence: {}",
                                     sourceFormat.precisionBits,
                                     domain,
                                     describeEvidence(evidence)))
                 .error(),
      .cause = cause}};
  }
} // namespace ao::audio::backend::detail
