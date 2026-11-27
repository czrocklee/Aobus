// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderOutputAdapter.h"

#include "DecoderOutput.h"
#include "PcmConversion.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::detail
{
  DecoderOutputAdapter::DecoderOutputAdapter(std::optional<SampleEncoding> optRequestedEncoding)
    : _optRequestedEncoding{optRequestedEncoding}
  {
  }

  Result<PcmFormat> DecoderOutputAdapter::configure(SignalFormat const& sourceFormat,
                                                    SampleEncoding const nativeEncoding)
  {
    if (!isLosslessPcmEncoding(sourceFormat, nativeEncoding))
    {
      return makeError(Error::Code::NotSupported, "Decoder native PCM encoding loses source precision");
    }

    auto const outputEncoding = _optRequestedEncoding.value_or(nativeEncoding);

    if (!isLosslessPcmEncoding(sourceFormat, outputEncoding))
    {
      return makeError(Error::Code::NotSupported, "Requested decoder PCM encoding loses source precision");
    }

    if (isFloatEncoding(nativeEncoding) && !isFloatEncoding(outputEncoding))
    {
      return makeError(
        Error::Code::NotSupported, "Decoder PCM adapter cannot convert floating-point output to integer PCM");
    }

    _sourceFormat = sourceFormat;
    _nativeFormat = pcmFormat(sourceFormat, nativeEncoding);
    _outputEncoding = outputEncoding;
    _convertedBytes.clear();
    return pcmFormat(sourceFormat, outputEncoding);
  }

  Result<std::span<std::byte const>> DecoderOutputAdapter::convert(std::span<std::byte const> nativeBytes)
  {
    AO_INVARIANT((_nativeFormat.encoding != SampleEncoding::Unknown && _outputEncoding != SampleEncoding::Unknown),
                 "Decoder PCM output adapter is not configured");

    if (_nativeFormat.encoding == _outputEncoding)
    {
      return nativeBytes;
    }

    if (auto convertedRes =
          convertPcmEncoding(nativeBytes, _nativeFormat, _sourceFormat, _outputEncoding, _convertedBytes);
        !convertedRes)
    {
      return std::unexpected{convertedRes.error()};
    }

    return std::span<std::byte const>{_convertedBytes};
  }

  PcmFormat const& DecoderOutputAdapter::nativeFormat() const noexcept
  {
    return _nativeFormat;
  }
} // namespace ao::audio::detail
