// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

namespace ao::uimodel
{
  struct LayoutValue;
  struct LayoutNode;
  struct LayoutDocument;
}

namespace ao::yaml
{
  void write(ryml::NodeRef node, uimodel::LayoutValue const& value);
  bool read(ryml::ConstNodeRef node, uimodel::LayoutValue& value);

  void write(ryml::NodeRef node, uimodel::LayoutNode const& value);
  bool read(ryml::ConstNodeRef node, uimodel::LayoutNode& value);

  void write(ryml::NodeRef node, uimodel::LayoutDocument const& value);
  bool read(ryml::ConstNodeRef node, uimodel::LayoutDocument& value);
} // namespace ao::yaml
