// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <string_view>

namespace ao::audio
{
  struct Format;
}

namespace ao::audio::detail
{
  Result<> validateFixedOutputRequest(Format const& requested, Format const& actual, std::string_view codecName);
} // namespace ao::audio::detail
