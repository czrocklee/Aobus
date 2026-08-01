// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/audio/NodeFormat.h>

#include <ao/audio/SignalFormat.h>

#include <variant>

namespace ao::audio
{
  SignalFormat signalFormat(NodeFormat const& format) noexcept
  {
    return std::visit([](auto const& value) { return signalFormat(value); }, format);
  }
} // namespace ao::audio
