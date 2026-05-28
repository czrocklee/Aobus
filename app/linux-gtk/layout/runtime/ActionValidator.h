// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"

#include <string>
#include <vector>

namespace ao::gtk::layout
{
  class ComponentRegistry;
  class ActionRegistry;

  struct LayoutDiagnostic final
  {
    std::string componentId;
    std::string propertyName;
    std::string actionId;
    std::string message;
  };

  /**
   * @brief Validates all action bindings in a layout document.
   * @return A list of diagnostics for invalid action bindings. Empty if valid.
   */
  std::vector<LayoutDiagnostic> validateActions(LayoutDocument const& doc,
                                                ComponentRegistry const& compRegistry,
                                                ActionRegistry const& actionRegistry);
} // namespace ao::gtk::layout
