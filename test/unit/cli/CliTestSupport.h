// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/yaml/RymlAdapter.h>

#include <string_view>

namespace ao::cli::test
{
  inline ryml::Tree parseYaml(std::string_view text)
  {
    auto state = yaml::ErrorCallbackState{};
    auto tree = ryml::Tree{yaml::callbacks(state)};
    yaml::parseInArena(tree, text, state);
    tree.callbacks(yaml::callbacks());
    return tree;
  }
} // namespace ao::cli::test
