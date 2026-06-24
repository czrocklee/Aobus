// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/GtkLayoutPresets.h"
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <functional>
#include <map>
#include <string>

namespace ao::gtk::layout
{
  using LayoutPresetId = GtkLayoutPresetId;

  inline uimodel::layout::LayoutDocument createDefaultLayout()
  {
    return createDefaultGtkLayout();
  }

  inline uimodel::layout::LayoutDocument createBuiltInLayout(LayoutPresetId presetId)
  {
    return createBuiltInGtkLayout(presetId);
  }

  inline std::map<std::string, uimodel::layout::LayoutNode, std::less<>> getBuiltInTemplates()
  {
    return getBuiltInGtkTemplates();
  }
} // namespace ao::gtk::layout
