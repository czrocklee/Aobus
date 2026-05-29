// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ActionValidator.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <optional>

namespace ao::gtk::layout
{
  using LayoutDiagnostic = uimodel::layout::LayoutDiagnostic;
  using uimodel::layout::validateActions;

  std::optional<uimodel::layout::ActionBindingContext> resolveGtkLayoutActionBindingContext(
    uimodel::layout::LayoutNode const& node,
    uimodel::layout::PropertyDescriptor const& property);
}
