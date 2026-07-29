// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>

#include <optional>
#include <string_view>

namespace ao
{
  std::string_view audioCodecName(AudioCodec codec) noexcept;
  std::optional<AudioCodec> parseAudioCodecName(std::string_view name) noexcept;
} // namespace ao
