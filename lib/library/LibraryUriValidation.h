// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string_view>

namespace ao::library::detail
{
  bool isCanonicalLibraryUri(std::string_view text) noexcept;
} // namespace ao::library::detail
