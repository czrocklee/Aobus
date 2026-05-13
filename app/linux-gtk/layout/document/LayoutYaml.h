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
  template<>
  struct convert<ao::gtk::layout::LayoutValue>
  {
    static Node encode(ao::gtk::layout::LayoutValue const& rhs);
    static bool decode(Node const& node, ao::gtk::layout::LayoutValue& rhs);
  };

  template<>
  struct convert<std::map<std::string, ao::gtk::layout::LayoutValue, std::less<>>>
  {
    static Node encode(std::map<std::string, ao::gtk::layout::LayoutValue, std::less<>> const& rhs)
    {
      auto node = Node{NodeType::Map};

      for (auto const& [key, value] : rhs)
      {
        node[key] = value;
      }

      return node;
    }

    static bool decode(Node const& node, std::map<std::string, ao::gtk::layout::LayoutValue, std::less<>>& rhs)
    {
      if (!node.IsMap())
      {
        return false;
      }

      rhs.clear();

      for (auto const& item : node)
      {
        rhs.emplace(item.first.as<std::string>(), item.second.as<ao::gtk::layout::LayoutValue>());
      }

      return true;
    }
  };

  template<>
  struct convert<ao::gtk::layout::LayoutNode>
  {
    static Node encode(ao::gtk::layout::LayoutNode const& rhs);
    static bool decode(Node const& node, ao::gtk::layout::LayoutNode& rhs);
  };

  template<>
  struct convert<ao::gtk::layout::LayoutDocument>
  {
    static Node encode(ao::gtk::layout::LayoutDocument const& rhs);
    static bool decode(Node const& node, ao::gtk::layout::LayoutDocument& rhs);
  };
} // namespace YAML
