// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

namespace ao::uimodel::layout
{
  struct LayoutValue;
  struct LayoutNode;
  struct LayoutDocument;
}

namespace ao::yaml
{
  void write(ryml::NodeRef node, uimodel::layout::LayoutValue const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutValue& value);

  void write(ryml::NodeRef node, uimodel::layout::LayoutNode const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutNode& value);

  void write(ryml::NodeRef node, uimodel::layout::LayoutDocument const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutDocument& value);
} // namespace ao::yaml
