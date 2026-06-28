// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutNode.h>

namespace ao::uimodel
{
  struct LayoutDocument;

  class LayoutTemplateExpander final
  {
  public:
    static LayoutNode expand(LayoutDocument const& doc);
  };
}
