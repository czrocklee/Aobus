// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SignalFormat.h>

#include <variant>

namespace ao::audio
{
  using NodeFormat = std::variant<SignalFormat, PcmFormat>;

  constexpr SignalFormat signalFormat(SignalFormat const& format) noexcept
  {
    return format;
  }

  SignalFormat signalFormat(NodeFormat const& format) noexcept;
} // namespace ao::audio
