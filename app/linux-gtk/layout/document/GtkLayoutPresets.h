// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  enum class GtkLayoutPresetId : std::uint8_t
  {
    Classic,
    Modern
  };

  GtkLayoutPresetId presetIdFromString(std::string_view presetIdStr);

  uimodel::layout::LayoutDocument createDefaultGtkLayout();

  uimodel::layout::LayoutDocument createBuiltInGtkLayout(GtkLayoutPresetId presetId);

  std::map<std::string, uimodel::layout::LayoutNode, std::less<>> getBuiltInGtkTemplates();
}
