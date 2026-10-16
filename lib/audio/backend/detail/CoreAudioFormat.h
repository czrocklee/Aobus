// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <CoreAudioTypes/CoreAudioBaseTypes.h>

#include <expected>
#include <functional>

namespace ao::audio::backend::detail
{
  using TryCoreAudioClientFormat = std::function<std::expected<::AudioStreamBasicDescription, BackendOpenFailure>(
    ::AudioStreamBasicDescription const&)>;

  Result<::AudioStreamBasicDescription> coreAudioFormat(PcmFormat const& format);

  Result<SignalFormat> coreAudioSignalFormat(::AudioStreamBasicDescription const& format);

  std::expected<PcmFormat, BackendOpenFailure> selectLosslessCoreAudioClientFormat(
    SignalFormat const& sourceFormat,
    TryCoreAudioClientFormat const& tryFormat);

  bool isSameCoreAudioPcmFormat(::AudioStreamBasicDescription const& lhs,
                                ::AudioStreamBasicDescription const& rhs) noexcept;
} // namespace ao::audio::backend::detail
