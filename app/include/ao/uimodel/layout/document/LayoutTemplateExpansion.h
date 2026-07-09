// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutNode.h>

namespace ao::uimodel
{
  struct LayoutDocument;

  LayoutNode expandLayoutTemplates(LayoutDocument const& doc);
}
