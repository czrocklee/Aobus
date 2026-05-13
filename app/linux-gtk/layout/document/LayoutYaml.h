// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"

#include <yaml-cpp/yaml.h>

#include <map>
#include <string>
#include <vector>

namespace YAML
{
  using namespace ao::gtk::layout;

  template<>
  struct convert<LayoutValue>
  {
    static Node encode(LayoutValue const& rhs);
    static bool decode(Node const& node, LayoutValue& rhs);
  };

  template<>
  struct convert<std::map<std::string, LayoutValue, std::less<>>>
  {
    static Node encode(std::map<std::string, LayoutValue, std::less<>> const& rhs)
    {
      auto node = Node{NodeType::Map};

      for (auto const& [key, value] : rhs)
      {
        node[key] = value;
      }

      return node;
    }

    static bool decode(Node const& node, std::map<std::string, LayoutValue, std::less<>>& rhs)
    {
      if (!node.IsMap())
      {
        return false;
      }

      rhs.clear();

      for (auto const& item : node)
      {
        rhs.emplace(item.first.as<std::string>(), item.second.as<LayoutValue>());
      }

      return true;
    }
  };

  template<>
  struct convert<LayoutNode>
  {
    static Node encode(LayoutNode const& rhs);
    static bool decode(Node const& node, LayoutNode& rhs);
  };

  template<>
  struct convert<LayoutDocument>
  {
    static Node encode(LayoutDocument const& rhs);
    static bool decode(Node const& node, LayoutDocument& rhs);
  };
} // namespace YAML
