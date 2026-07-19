// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    using ReservedLayoutNodeIds =
      boost::unordered_flat_set<std::string, utility::TransparentStringHash, utility::TransparentStringEqual>;
    using LayoutNodeTypesById = boost::
      unordered_flat_map<std::string, std::string, utility::TransparentStringHash, utility::TransparentStringEqual>;

    void visitNodeRecursive(LayoutNode const& node, LayoutNodeVisitor const& visitor)
    {
      visitor(node);

      for (auto const& child : node.children)
      {
        visitNodeRecursive(child, visitor);
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        visitNodeRecursive(*node.optTooltip->nodePtr, visitor);
      }
    }

    void collectIds(LayoutDocument const& doc, ReservedLayoutNodeIds& reservedIds)
    {
      visitLayoutDocumentNodes(doc,
                               [&reservedIds](LayoutNode const& node)
                               {
                                 if (!node.id.empty())
                                 {
                                   reservedIds.insert(node.id);
                                 }
                               });
    }

    std::string idStem(std::string_view componentType, std::string_view role)
    {
      auto result = std::string{};

      auto appendSanitized = [&result](std::string_view text)
      {
        bool previousWasSeparator = false;

        for (char const ch : text)
        {
          if (auto const uch = static_cast<unsigned char>(ch); std::isalnum(uch) != 0)
          {
            result.push_back(static_cast<char>(std::tolower(uch)));
            previousWasSeparator = false;
          }
          else if (!previousWasSeparator && !result.empty())
          {
            result.push_back('-');
            previousWasSeparator = true;
          }
        }

        while (!result.empty() && result.back() == '-')
        {
          result.pop_back();
        }
      };

      appendSanitized(componentType);

      if (!role.empty())
      {
        if (!result.empty())
        {
          result.push_back('-');
        }

        appendSanitized(role);
      }

      if (result.empty())
      {
        return "component";
      }

      return result;
    }

    std::string makeUniqueFromReserved(ReservedLayoutNodeIds& reservedIds, std::string const& stem)
    {
      if (!reservedIds.contains(stem))
      {
        reservedIds.insert(stem);
        return stem;
      }

      for (std::int32_t index = 2; true; ++index)
      {
        if (auto candidate = stem + "-" + std::to_string(index); !reservedIds.contains(candidate))
        {
          reservedIds.insert(candidate);
          return candidate;
        }
      }
    }

    void regenerateRecursive(LayoutNode& node, ReservedLayoutNodeIds& reservedIds)
    {
      if (!node.id.empty() || isStatefulLayoutComponentType(node.type))
      {
        auto const stem = idStem(node.type, node.id.empty() ? "copy" : node.id);
        node.id = makeUniqueFromReserved(reservedIds, stem);
      }

      for (auto& child : node.children)
      {
        regenerateRecursive(child, reservedIds);
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        regenerateRecursive(*node.optTooltip->nodePtr, reservedIds);
      }
    }
  } // namespace

  void visitLayoutNode(LayoutNode const& node, LayoutNodeVisitor const& visitor)
  {
    visitNodeRecursive(node, visitor);
  }

  void visitLayoutDocumentNodes(LayoutDocument const& doc, LayoutNodeVisitor const& visitor)
  {
    visitNodeRecursive(doc.root, visitor);

    for (auto const& [templateId, templateNode] : doc.templates)
    {
      std::ignore = templateId;
      visitNodeRecursive(templateNode, visitor);
    }
  }

  void visitExpandedLayoutNodes(PreparedLayout const& layout, LayoutNodeVisitor const& visitor)
  {
    visitNodeRecursive(layout.effectiveRoot(), visitor);
  }

  std::vector<LayoutNodeIdDiagnostic> validateStatefulLayoutNodeIds(PreparedLayout const& layout)
  {
    auto diagnostics = std::vector<LayoutNodeIdDiagnostic>{};
    auto seenNodeTypesById = LayoutNodeTypesById{};

    visitExpandedLayoutNodes(
      layout,
      [&diagnostics, &seenNodeTypesById](LayoutNode const& node)
      {
        if (!isStatefulLayoutComponentType(node.type))
        {
          return;
        }

        if (node.id.empty())
        {
          diagnostics.push_back(LayoutNodeIdDiagnostic{
            .severity = LayoutNodeIdDiagnosticSeverity::Warning,
            .componentId = node.type,
            .componentType = node.type,
            .message = "Stateful layout component has no id; runtime state will not be persisted"});
          return;
        }

        if (auto const [it, inserted] = seenNodeTypesById.emplace(node.id, node.type); !inserted)
        {
          diagnostics.push_back(LayoutNodeIdDiagnostic{
            .severity = LayoutNodeIdDiagnosticSeverity::Error,
            .componentId = node.id,
            .componentType = node.type,
            .message = "Duplicate stateful layout component id also used by type '" + it->second + "'"});
        }
      });

    return diagnostics;
  }

  bool hasLayoutNodeIdErrors(std::vector<LayoutNodeIdDiagnostic> const& diagnostics)
  {
    return std::ranges::any_of(diagnostics,
                               [](LayoutNodeIdDiagnostic const& diagnostic)
                               { return diagnostic.severity == LayoutNodeIdDiagnosticSeverity::Error; });
  }

  std::string makeUniqueLayoutNodeId(LayoutDocument const& doc, std::string_view componentType, std::string_view role)
  {
    auto reservedIds = ReservedLayoutNodeIds{};
    collectIds(doc, reservedIds);
    return makeUniqueFromReserved(reservedIds, idStem(componentType, role));
  }

  void regenerateLayoutNodeIds(LayoutNode& subtree, LayoutDocument const& ownerDoc)
  {
    auto reservedIds = ReservedLayoutNodeIds{};
    collectIds(ownerDoc, reservedIds);
    regenerateRecursive(subtree, reservedIds);
  }
} // namespace ao::uimodel
