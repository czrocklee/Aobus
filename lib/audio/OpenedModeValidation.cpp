// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "detail/OpenedModeValidation.h"

#include "detail/DecoderOutput.h"
#include <ao/Error.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstdint>
#include <format>

namespace ao::audio::detail
{
  Result<> validateOpenedMode(SignalFormat const& sourceFormat, OpenedPcmMode const& mode)
  {
    auto const& client = mode.clientFormat;

    if (client.sampleRate != sourceFormat.sampleRate || client.channels != sourceFormat.channels)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Backend returned {} Hz / {} ch for a {} Hz / {} ch source",
                                   client.sampleRate,
                                   static_cast<std::uint32_t>(client.channels),
                                   sourceFormat.sampleRate,
                                   static_cast<std::uint32_t>(sourceFormat.channels)));
    }

    if (!isLosslessPcmEncoding(sourceFormat, client.encoding))
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Backend returned lossy {} for a {}-bit source",
                                   sampleEncodingName(client.encoding),
                                   static_cast<std::uint32_t>(sourceFormat.precisionBits)));
    }

    if (!mode.optEndpoint)
    {
      return {};
    }

    auto const& endpoint = mode.optEndpoint->signalFormat;

    if (endpoint.sampleRate != sourceFormat.sampleRate || endpoint.channels != sourceFormat.channels)
    {
      return makeError(Error::Code::FormatRejected, "Backend confirmed an endpoint with a different rate or layout");
    }

    if (endpoint.precisionBits == 0U || endpoint.precisionBits > encodingNominalBits(client.encoding))
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Backend confirmed a {}-bit endpoint behind {}, which cannot carry it",
                                   static_cast<std::uint32_t>(endpoint.precisionBits),
                                   sampleEncodingName(client.encoding)));
    }

    auto const clientKind = isFloatEncoding(client.encoding) ? SampleKind::FloatingPoint : SampleKind::Integer;

    if (endpoint.sampleKind != clientKind)
    {
      return makeError(Error::Code::FormatRejected, "Backend confirmed an endpoint in a different sample domain");
    }

    if (endpoint.precisionBits < sourceFormat.precisionBits)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Backend confirmed a {}-bit endpoint for a {}-bit source",
                                   static_cast<std::uint32_t>(endpoint.precisionBits),
                                   static_cast<std::uint32_t>(sourceFormat.precisionBits)));
    }

    return {};
  }
} // namespace ao::audio::detail
