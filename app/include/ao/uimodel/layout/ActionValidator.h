// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::uimodel::layout
{
  class ActionCatalog;
  struct LayoutDocument;

  struct LayoutDiagnostic final
  {
    std::string componentId;
    std::string propertyName;
    std::string actionId;
    std::string message;
  };

  using ActionBindingContextResolver =
    std::function<std::optional<ActionBindingContext>(LayoutNode const& node, PropertyDescriptor const& property)>;

  std::vector<LayoutDiagnostic> validateActions(LayoutDocument const& doc,
                                                ComponentCatalog const& components,
                                                ActionCatalog const& actions,
                                                ActionBindingContextResolver const& resolveBindingContext);
} // namespace ao::uimodel::layout
