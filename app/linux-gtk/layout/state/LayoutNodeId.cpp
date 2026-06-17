// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/state/LayoutNodeId.h"

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"
#include "layout/state/StatefulLayoutComponentType.h"
#include <ao/uimodel/layout/LayoutTemplateExpander.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
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

    void collectIds(LayoutDocument const& doc, std::set<std::string, std::less<>>& ids)
    {
      visitLayoutDocumentNodes(doc,
                               [&ids](LayoutNode const& node)
                               {
                                 if (!node.id.empty())
                                 {
                                   ids.insert(node.id);
                                 }
                               });
    }

    std::string idStem(std::string_view componentType, std::string_view role)
    {
      auto result = std::string{};

      auto appendSanitized = [&result](std::string_view text)
      {
        auto previousWasSeparator = false;

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

    std::string makeUniqueFromReserved(std::set<std::string, std::less<>>& reserved, std::string const& stem)
    {
      if (!reserved.contains(stem))
      {
        reserved.insert(stem);
        return stem;
      }

      for (auto index = 2; true; ++index)
      {
        if (auto candidate = stem + "-" + std::to_string(index); !reserved.contains(candidate))
        {
          reserved.insert(candidate);
          return candidate;
        }
      }
    }

    void freshenRecursive(LayoutNode& node, std::set<std::string, std::less<>>& reserved)
    {
      if (!node.id.empty() || isStatefulLayoutComponentType(node.type))
      {
        auto const stem = idStem(node.type, node.id.empty() ? "copy" : node.id);
        node.id = makeUniqueFromReserved(reserved, stem);
      }

      for (auto& child : node.children)
      {
        freshenRecursive(child, reserved);
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        freshenRecursive(*node.optTooltip->nodePtr, reserved);
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

  void visitExpandedLayoutNodes(LayoutDocument const& doc, LayoutNodeVisitor const& visitor)
  {
    auto const expandedRoot = uimodel::layout::LayoutTemplateExpander::expand(doc);
    visitNodeRecursive(expandedRoot, visitor);
  }

  std::vector<LayoutNodeIdDiagnostic> validateStatefulLayoutNodeIds(LayoutDocument const& doc)
  {
    auto diagnostics = std::vector<LayoutNodeIdDiagnostic>{};
    auto seenIds = std::map<std::string, std::string, std::less<>>{};

    visitExpandedLayoutNodes(
      doc,
      [&diagnostics, &seenIds](LayoutNode const& node)
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

        if (auto const [it, inserted] = seenIds.emplace(node.id, node.type); !inserted)
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
    auto reserved = std::set<std::string, std::less<>>{};
    collectIds(doc, reserved);
    return makeUniqueFromReserved(reserved, idStem(componentType, role));
  }

  void freshenLayoutNodeIds(LayoutNode& subtree, LayoutDocument const& ownerDoc)
  {
    auto reserved = std::set<std::string, std::less<>>{};
    collectIds(ownerDoc, reserved);
    freshenRecursive(subtree, reserved);
  }
} // namespace ao::gtk::layout
