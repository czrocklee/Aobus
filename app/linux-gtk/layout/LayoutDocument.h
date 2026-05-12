// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "LayoutNode.h"

#include <cstdint>
#include <map>
#include <string>

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
}
