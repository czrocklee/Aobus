// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <AudioToolbox/AudioToolbox.h>

#include <functional>

namespace ao::audio::backend::detail
{
  using TryCoreAudioClientFormat =
    std::function<Result<::AudioStreamBasicDescription>(::AudioStreamBasicDescription const&)>;

  Result<::AudioStreamBasicDescription> coreAudioFormat(PcmFormat const& format);

  Result<SignalFormat> coreAudioSignalFormat(::AudioStreamBasicDescription const& format);

  Result<PcmFormat> selectLosslessCoreAudioClientFormat(
    SignalFormat const& sourceFormat,
    TryCoreAudioClientFormat const& tryFormat);

  bool sameCoreAudioPcmFormat(::AudioStreamBasicDescription const& lhs,
                              ::AudioStreamBasicDescription const& rhs) noexcept;
} // namespace ao::audio::backend::detail
