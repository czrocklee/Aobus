// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

namespace ao::uimodel::layout
{
  struct LayoutComponentStateEntry;
  struct LayoutComponentStateDocument;
} // namespace ao::uimodel::layout

namespace ao::yaml
{
  void write(ryml::NodeRef node, uimodel::layout::LayoutComponentStateEntry const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutComponentStateEntry& value);

  void write(ryml::NodeRef node, uimodel::layout::LayoutComponentStateDocument const& value);
  bool read(ryml::ConstNodeRef node, uimodel::layout::LayoutComponentStateDocument& value);
} // namespace ao::yaml
