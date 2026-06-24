// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionValidator.h"

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <optional>

namespace ao::gtk::layout
{
  std::optional<uimodel::layout::ActionBindingContext> resolveGtkLayoutActionBindingContext(
    uimodel::layout::LayoutNode const& node,
    uimodel::layout::PropertyDescriptor const& property)
  {
    if (!property.optActionBinding)
    {
      return std::nullopt;
    }

    return uimodel::layout::ActionBindingContext{
      .slot = property.optActionBinding->slot, .hasAnchor = true, .hasFocusedView = true, .componentType = node.type};
  }
} // namespace ao::gtk::layout
