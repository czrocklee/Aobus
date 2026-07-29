// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>

namespace ao::audio::backend::detail
{
  void addSampleFormatCapability(DeviceFormatCapabilities& caps, SampleFormatCapability const& capability);
  Format preserveRequestedSignalPrecision(Format const& requested, Format current) noexcept;
} // namespace ao::audio::backend::detail
