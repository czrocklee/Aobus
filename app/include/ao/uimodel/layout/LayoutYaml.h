// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <ryml.hpp>

namespace ao::rt::yaml
{
  void write(ryml::NodeRef node, uimodel::layout::LayoutValue const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutValue& value);

  void write(ryml::NodeRef node, uimodel::layout::LayoutNode const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutNode& value);

  void write(ryml::NodeRef node, uimodel::layout::LayoutDocument const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutDocument& value);
}
