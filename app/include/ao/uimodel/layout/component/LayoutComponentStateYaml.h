// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ryml.hpp>

namespace ao::uimodel
{
  struct LayoutComponentStateEntry;
  struct LayoutComponentStateDocument;
} // namespace ao::uimodel

namespace ao::yaml
{
  void write(ryml::NodeRef node, uimodel::LayoutComponentStateEntry const& value);
  bool read(ryml::ConstNodeRef node, uimodel::LayoutComponentStateEntry& value);

  void write(ryml::NodeRef node, uimodel::LayoutComponentStateDocument const& value);
  bool read(ryml::ConstNodeRef node, uimodel::LayoutComponentStateDocument& value);
} // namespace ao::yaml
