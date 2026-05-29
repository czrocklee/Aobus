// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

namespace ao::uimodel::layout
{
  class LayoutTemplateExpander final
  {
  public:
    static LayoutNode expand(LayoutDocument const& doc);
  };
}
