// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <span>
#include <string_view>

namespace ao::library::test
{
  struct LibraryFatalProbeExpectation final
  {
    std::string_view scenario;
    std::string_view category;
    std::string_view condition;
    std::string_view context;
    std::string_view source;
    std::string_view function;
    bool needsScratchDirectory = false;
  };

  std::span<LibraryFatalProbeExpectation const> libraryFatalProbeExpectations() noexcept;
} // namespace ao::library::test
