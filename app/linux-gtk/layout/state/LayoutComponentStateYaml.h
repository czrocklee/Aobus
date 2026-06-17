// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

namespace ao::gtk::layout
{
  struct LayoutComponentStateEntry;
  struct LayoutComponentStateDocument;
} // namespace ao::gtk::layout

namespace ao::yaml
{
  void write(ryml::NodeRef node, gtk::layout::LayoutComponentStateEntry const& value);
  bool read(ryml::ConstNodeRef node, gtk::layout::LayoutComponentStateEntry& value);

  void write(ryml::NodeRef node, gtk::layout::LayoutComponentStateDocument const& value);
  bool read(ryml::ConstNodeRef node, gtk::layout::LayoutComponentStateDocument& value);
} // namespace ao::yaml
