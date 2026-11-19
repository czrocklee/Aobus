// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "platform/StringResources.h"

#include <format>
#include <string>
#include <string_view>

namespace ao::winui
{
  // Formats canonical MRT projections whose named ICU argument is generated as {0}.
  template<typename... Args>
  std::string formatResource(std::string_view const resourceId, Args const&... args)
  {
    auto const pattern = resourceString(resourceId);
    return std::vformat(pattern, std::make_format_args(args...));
  }
} // namespace ao::winui
