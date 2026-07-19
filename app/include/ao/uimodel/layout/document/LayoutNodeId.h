// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  struct LayoutDocument;
  class PreparedLayout;
  enum class LayoutNodeIdDiagnosticSeverity : std::uint8_t
  {
    Warning,
    Error
  };

  struct LayoutNodeIdDiagnostic final
  {
    LayoutNodeIdDiagnosticSeverity severity = LayoutNodeIdDiagnosticSeverity::Warning;
    std::string componentId;
    std::string componentType;
    std::string message;
  };

  using LayoutNodeVisitor = std::function<void(LayoutNode const&)>;

  void visitLayoutNode(LayoutNode const& node, LayoutNodeVisitor const& visitor);
  void visitLayoutDocumentNodes(LayoutDocument const& doc, LayoutNodeVisitor const& visitor);
  void visitExpandedLayoutNodes(PreparedLayout const& layout, LayoutNodeVisitor const& visitor);

  std::vector<LayoutNodeIdDiagnostic> validateStatefulLayoutNodeIds(PreparedLayout const& layout);
  bool hasLayoutNodeIdErrors(std::vector<LayoutNodeIdDiagnostic> const& diagnostics);

  std::string makeUniqueLayoutNodeId(LayoutDocument const& doc, std::string_view componentType, std::string_view role);
  void regenerateLayoutNodeIds(LayoutNode& subtree, LayoutDocument const& ownerDoc);
} // namespace ao::uimodel
