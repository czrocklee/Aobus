// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::uimodel
{
  struct LayoutDocument final
  {
    std::uint32_t version = 1;
    LayoutNode root{};
    std::map<std::string, LayoutNode, std::less<>> templates{};
  };

  Result<> loadLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument& doc);

  Result<> saveLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument const& doc);
} // namespace ao::uimodel
