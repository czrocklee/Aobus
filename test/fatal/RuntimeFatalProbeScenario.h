// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::rt::test
{
  std::int32_t runRuntimeFatalProbeScenario(std::string_view scenario);
} // namespace ao::rt::test
