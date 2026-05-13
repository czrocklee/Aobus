// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutRuntime.h"
#include "layout/components/Containers.h"
#include "layout/components/PlaybackComponents.h"
#include "layout/components/SemanticComponents.h"

#include <algorithm>
#include <vector>

namespace ao::gtk::layout
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

        if (std::ranges::find(visited, templateId) != visited.end())
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

        return expanded;
      }

      auto result = node;
      result.children.clear();

      for (auto const& child : node.children)
      {
        result.children.push_back(expandNode(child, templates, visited));
      }

      return result;
    }
  }

  LayoutRuntime::LayoutRuntime(ComponentRegistry const& registry)
    : _registry{registry}
  {
  }

  void LayoutRuntime::registerStandardComponents(ComponentRegistry& registry)
  {
    registerContainerComponents(registry);
    registerPlaybackComponents(registry);
    registerSemanticComponents(registry);
  }

  std::unique_ptr<ILayoutComponent> LayoutRuntime::build(LayoutDependencies& ctx, LayoutDocument const& doc)
  {
    auto visited = std::vector<std::string>{};
    auto const expandedRoot = expandNode(doc.root, doc.templates, visited);
    return _registry.create(ctx, expandedRoot);
  }
} // namespace ao::gtk::layout
