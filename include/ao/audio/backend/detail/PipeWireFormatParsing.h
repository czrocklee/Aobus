// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>

extern "C"
{
#include <spa/pod/pod.h>
}

#include <optional>

namespace ao::audio::backend::detail
{
  std::optional<Format> parseRawStreamFormat(::spa_pod const* param) noexcept;
} // namespace ao::audio::backend::detail
