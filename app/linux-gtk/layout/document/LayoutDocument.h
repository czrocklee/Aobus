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

  inline uimodel::LayoutDocument makeDefaultLayout()
  {
    return makeDefaultGtkLayout();
  }

  inline uimodel::LayoutDocument makeBuiltInLayout(LayoutPresetId presetId)
  {
    return makeBuiltInGtkLayout(presetId);
  }

  inline std::map<std::string, uimodel::LayoutNode, std::less<>> builtInTemplates()
  {
    return builtInGtkTemplates();
  }
} // namespace ao::gtk::layout
