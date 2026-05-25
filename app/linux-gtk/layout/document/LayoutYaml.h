// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"
#include <ao/rt/yaml/Utils.h> // NOLINT(misc-include-cleaner)

namespace ao::rt::yaml
{
  void write(ryml::NodeRef node, gtk::layout::LayoutValue const& value);
  bool read(ryml::ConstNodeRef node, gtk::layout::LayoutValue& value);

  void write(ryml::NodeRef node, gtk::layout::LayoutNode const& value);
  bool read(ryml::ConstNodeRef node, gtk::layout::LayoutNode& value);

  void write(ryml::NodeRef node, gtk::layout::LayoutDocument const& value);
  bool read(ryml::ConstNodeRef node, gtk::layout::LayoutDocument& value);
} // namespace ao::rt::yaml
