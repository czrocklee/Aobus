// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/GtkLayoutPresets.h"
#include "layout/document/LayoutNode.h"
#include <ao/uimodel/layout/LayoutDocument.h>

#include <functional>
#include <map>
#include <string>

namespace ao::gtk::layout
{
  using uimodel::layout::LayoutDocument;
  using uimodel::layout::loadLayout;
  using uimodel::layout::saveLayout;

  using LayoutPresetId = GtkLayoutPresetId;

  inline LayoutDocument createDefaultLayout()
  {
    return createDefaultGtkLayout();
  }

  inline LayoutDocument createBuiltInLayout(LayoutPresetId presetId)
  {
    return createBuiltInGtkLayout(presetId);
  }

  inline std::map<std::string, LayoutNode, std::less<>> getBuiltInTemplates()
  {
    return getBuiltInGtkTemplates();
  }
}
