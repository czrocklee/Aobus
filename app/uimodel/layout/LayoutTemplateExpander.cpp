// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutTemplateExpander.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ao::uimodel::layout
{
  namespace
  {
    LayoutNode expandNode(LayoutNode const& node,
                          std::map<std::string, LayoutNode, std::less<>> const& templates,
                          std::vector<std::string>& visited)
    {
      if (node.type == "template")
      {
        auto const templateId = node.getProp<std::string>("templateId", "");

        if (templateId.empty())
        {
          return LayoutNode{.id = node.id, .type = "[TemplateError] Missing templateId"};
        }

        if (std::ranges::contains(visited, templateId))
        {
          auto chain = std::string{};

          for (auto const& visitedId : visited)
          {
            chain += visitedId + " -> ";
          }

          chain += templateId;
          return LayoutNode{.type = "[TemplateError] Recursive template loop: " + chain};
        }

        auto const it = templates.find(templateId);

        if (it == templates.end())
        {
          return LayoutNode{.type = "[TemplateError] Unknown template: " + templateId};
        }

        visited.push_back(templateId);
        auto expanded = expandNode(it->second, templates, visited);
        visited.pop_back();

        if (!node.id.empty())
        {
          expanded.id = node.id;
        }

        for (auto const& [key, value] : node.layout)
        {
          expanded.layout[key] = value;
        }

        for (auto const& [key, value] : node.props)
        {
          if (key != "templateId")
          {
            expanded.props[key] = value;
          }
        }

        for (auto const& child : node.children)
        {
          expanded.children.push_back(expandNode(child, templates, visited));
        }

        if (node.optTooltip && node.optTooltip->nodePtr)
        {
          expanded.optTooltip = BoxedLayoutNode{expandNode(*node.optTooltip->nodePtr, templates, visited)};
        }

        return expanded;
      }

      auto result = LayoutNode{node};
      result.children.clear();

      for (auto const& child : node.children)
      {
        result.children.push_back(expandNode(child, templates, visited));
      }

      if (result.optTooltip && result.optTooltip->nodePtr)
      {
        result.optTooltip = BoxedLayoutNode{expandNode(*result.optTooltip->nodePtr, templates, visited)};
      }

      return result;
    }
  } // namespace

  LayoutNode LayoutTemplateExpander::expand(LayoutDocument const& doc)
  {
    auto visited = std::vector<std::string>{};
    return expandNode(doc.root, doc.templates, visited);
  }
} // namespace ao::uimodel::layout
