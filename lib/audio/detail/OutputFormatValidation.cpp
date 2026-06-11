// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OutputFormatValidation.h"

#include <ao/Error.h>
#include <ao/audio/Format.h>

#include <format>
#include <string_view>

namespace ao::audio::detail
{
  Result<> validateFixedOutputRequest(Format const& requested, Format const& actual, std::string_view codecName)
  {
    if (!requested.isInterleaved)
    {
      return makeError(Error::Code::NotSupported, std::format("{} planar output is not supported", codecName));
    }

    if (requested.sampleRate != 0 && actual.sampleRate != 0 && requested.sampleRate != actual.sampleRate)
    {
      return makeError(Error::Code::NotSupported, std::format("{} sample rate conversion is not supported", codecName));
    }

    if (requested.channels != 0 && actual.channels != 0 && requested.channels != actual.channels)
    {
      return makeError(Error::Code::NotSupported, std::format("{} channel remapping is not supported", codecName));
    }

    return {};
  }
} // namespace ao::audio::detail
