// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace ao::uimodel
{
  using TrackFieldEditValue = std::variant<std::monostate, std::string, std::uint16_t>;

  TrackFieldEditValue makeTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value);
  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value);
} // namespace ao::uimodel
