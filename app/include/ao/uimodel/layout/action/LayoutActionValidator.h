// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::uimodel
{
  class LayoutActionCatalog;
  struct LayoutDocument;

  struct LayoutDiagnostic final
  {
    std::string componentId;
    std::string propertyName;
    std::string actionId;
    std::string message;
  };

  using LayoutActionBindingContextResolver =
    std::function<std::optional<LayoutActionBindingContext>(LayoutNode const& node,
                                                            LayoutPropertyDescriptor const& property)>;

  std::vector<LayoutDiagnostic> validateLayoutActions(LayoutDocument const& doc,
                                                      LayoutComponentCatalog const& components,
                                                      LayoutActionCatalog const& actions,
                                                      LayoutActionBindingContextResolver const& resolveBindingContext);
} // namespace ao::uimodel
