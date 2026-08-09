// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <span>
#include <string_view>

namespace ao::rt::test
{
  struct RuntimeFatalProbeExpectation final
  {
    std::string_view scenario;
    std::string_view category;
    std::string_view condition;
    std::string_view context;
    std::string_view source;
    std::string_view function;
    std::string_view marker;
    std::string_view secondMarker;
  };

  std::span<RuntimeFatalProbeExpectation const> runtimeFatalProbeExpectations() noexcept;
  std::span<std::string_view const> runtimeCleanProbeScenarios() noexcept;
} // namespace ao::rt::test
