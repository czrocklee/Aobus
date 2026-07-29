// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

#include <string_view>

namespace ao::cli::test
{
  ryml::Tree parseYaml(std::string_view text);
} // namespace ao::cli::test
