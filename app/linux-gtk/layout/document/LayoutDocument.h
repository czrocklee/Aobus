// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "GtkLayoutPresets.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <functional>
#include <map>
#include <string>

namespace ao::gtk::layout
{
  using LayoutPresetId = GtkLayoutPresetId;

  inline uimodel::LayoutDocument createDefaultLayout()
  {
    return createDefaultGtkLayout();
  }

  inline uimodel::LayoutDocument createBuiltInLayout(LayoutPresetId presetId)
  {
    return createBuiltInGtkLayout(presetId);
  }

  inline std::map<std::string, uimodel::LayoutNode, std::less<>> getBuiltInTemplates()
  {
    return getBuiltInGtkTemplates();
  }
} // namespace ao::gtk::layout
