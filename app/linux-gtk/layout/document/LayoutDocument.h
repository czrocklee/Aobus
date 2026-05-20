// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "layout/document/LayoutNode.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk::layout
{
  /**
   * @brief Top-level layout document.
   */
  struct LayoutDocument final
  {
    std::uint32_t version = 1;
    LayoutNode root{};
    std::map<std::string, LayoutNode, std::less<>> templates{};
  };

  /**
   * @brief Create a built-in default layout document.
   */
  LayoutDocument createDefaultLayout();

  /**
   * @brief Get a map of all built-in layout templates.
   */
  std::map<std::string, LayoutNode, std::less<>> getBuiltInTemplates();

  /**
   * @brief Load a layout document from a config store.
   */
  Result<> loadLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument& doc);

  /**
   * @brief Save a layout document to a config store.
   */
  void saveLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument const& doc);
}
