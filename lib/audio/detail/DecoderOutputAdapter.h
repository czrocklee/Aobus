// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::detail
{
  class DecoderOutputAdapter final
  {
  public:
    explicit DecoderOutputAdapter(std::optional<SampleEncoding> optRequestedEncoding);

    Result<PcmFormat> configure(SignalFormat const& sourceFormat, SampleEncoding nativeEncoding);
    Result<std::span<std::byte const>> convert(std::span<std::byte const> nativeBytes);

    PcmFormat const& nativeFormat() const noexcept;

  private:
    std::optional<SampleEncoding> _optRequestedEncoding;
    SignalFormat _sourceFormat{};
    PcmFormat _nativeFormat{};
    SampleEncoding _outputEncoding = SampleEncoding::Unknown;
    std::vector<std::byte> _convertedBytes{};
  };
} // namespace ao::audio::detail
