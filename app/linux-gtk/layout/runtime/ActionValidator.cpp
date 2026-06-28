// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionValidator.h"

#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <optional>

namespace ao::gtk::layout
{
  std::optional<uimodel::LayoutActionBindingContext> resolveGtkLayoutActionBindingContext(
    uimodel::LayoutNode const& node,
    uimodel::LayoutPropertyDescriptor const& property)
  {
    if (!property.optActionBinding)
    {
      return std::nullopt;
    }

    return uimodel::LayoutActionBindingContext{
      .slot = property.optActionBinding->slot, .hasAnchor = true, .hasFocusedView = true, .componentType = node.type};
  }
} // namespace ao::gtk::layout
