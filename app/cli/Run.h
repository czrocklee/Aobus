// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ao::cli
{
  std::int32_t run(std::int32_t argc, char const* const* argv, std::ostream& out, std::ostream& err);
  std::int32_t run(std::vector<std::string> const& args, std::ostream& out, std::ostream& err);
} // namespace ao::cli
