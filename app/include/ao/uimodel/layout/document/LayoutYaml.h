// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <ryml.hpp>

#include <string_view>

namespace ao::uimodel
{
  Result<> writeLayoutValue(ryml::NodeRef node, LayoutValue const& value);
  Result<LayoutValue> readLayoutValue(ryml::ConstNodeRef node, std::string_view context);

  Result<> writeLayoutValueMap(ryml::NodeRef node, LayoutValueMap const& values);
  Result<LayoutValueMap> readLayoutValueMap(ryml::ConstNodeRef node, std::string_view context);

  Result<> writeLayoutNode(ryml::NodeRef node, LayoutNode const& value);
  Result<LayoutNode> readLayoutNode(ryml::ConstNodeRef node, std::string_view context);

  struct LayoutDocumentYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, LayoutDocument const& document) const;
    Result<LayoutDocument> deserialize(ryml::ConstNodeRef node, LayoutDocument const& seed) const;
  };
} // namespace ao::uimodel
