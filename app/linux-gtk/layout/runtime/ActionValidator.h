// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <optional>

namespace ao::gtk::layout
{
  std::optional<uimodel::LayoutActionBindingContext> resolveGtkLayoutActionBindingContext(
    uimodel::LayoutNode const& node,
    uimodel::LayoutPropertyDescriptor const& property);
}
