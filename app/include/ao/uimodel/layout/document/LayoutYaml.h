// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <ryml.hpp>

#include <cstdint>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutDocument;

  Result<> writeLayoutValue(ryml::NodeRef node, LayoutValue const& value);
  Result<LayoutValue> readLayoutValue(ryml::ConstNodeRef node, std::string_view context);

  Result<> writeLayoutValueMap(ryml::NodeRef node, LayoutValueMap const& values);
  Result<LayoutValueMap> readLayoutValueMap(ryml::ConstNodeRef node, std::string_view context);

  Result<> writeLayoutNode(ryml::NodeRef node, LayoutNode const& value);
  Result<LayoutNode> readLayoutNode(ryml::ConstNodeRef node, std::string_view context);

  /// A shell layout preset is protected configuration (decision 0020): an older build never overwrites a newer one.
  struct LayoutDocumentYamlSchema final
  {
    static constexpr std::uint32_t kVersion = kLayoutDocumentVersion;

    Result<> serialize(ryml::NodeRef node, LayoutDocument const& document) const;
    Result<LayoutDocument> deserialize(ryml::ConstNodeRef node, LayoutDocument const& seed) const;
  };
} // namespace ao::uimodel
