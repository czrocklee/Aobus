// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"
#include "layout/document/LayoutYaml.h"
#include <ao/utility/Log.h>

#include <giomm/resource.h>
#include <glib.h> // NOLINT(misc-include-cleaner)
#include <glibmm/error.h>
#include <yaml-cpp/yaml.h>

#include <charconv>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace YAML
{
  using namespace ao::gtk::layout;

  Node convert<LayoutValue>::encode(LayoutValue const& rhs)
  {
    return std::visit(
      [](auto const& nodeValue) -> Node
      {
        using T = std::decay_t<decltype(nodeValue)>;

        if constexpr (std::is_same_v<T, std::monostate>)
        {
          return Node(NodeType::Null);
        }
        else
        {
          return Node(nodeValue);
        }
      },
      rhs.data);
  }

  bool convert<LayoutValue>::decode(Node const& node, LayoutValue& rhs)
  {
    if (!node.IsDefined() || node.IsNull())
    {
      rhs.data = std::monostate{};
      return true;
    }

    if (node.IsScalar())
    {
      auto const& scalar = node.Scalar();

      {
        if (scalar == "true")
        {
          rhs.data = true;
          return true;
        }

        if (scalar == "false")
        {
          rhs.data = false;
          return true;
        }
      }

      auto const* const first = scalar.data();
      auto const* const last = first + scalar.size();

      {
        std::int64_t intValue = 0;

        if (auto const intResult = std::from_chars(first, last, intValue);
            intResult.ec == std::errc{} && intResult.ptr == last)
        {
          rhs.data = intValue;
          return true;
        }

        APP_LOG_TRACE("LayoutDocument: Failed to parse scalar '{}' as integer, trying double", scalar);
      }

      {
        double doubleValue = 0.0;

        if (auto const doubleResult = std::from_chars(first, last, doubleValue);
            doubleResult.ec == std::errc{} && doubleResult.ptr == last)
        {
          rhs.data = doubleValue;
          return true;
        }

        APP_LOG_TRACE("LayoutDocument: Failed to parse scalar '{}' as numeric, keeping as string", scalar);

        rhs.data = scalar;
        return true;
      }
    }

    if (node.IsSequence())
    {
      auto sequence = std::vector<std::string>{};

      for (auto const& item : node)
      {
        if (item.IsScalar())
        {
          sequence.push_back(item.as<std::string>());
        }
      }

      rhs.data = std::move(sequence);
      return true;
    }

    return false;
  }

  Node convert<LayoutNode>::encode(LayoutNode const& rhs)
  {
    auto node = Node{};

    if (!rhs.id.empty())
    {
      node["id"] = rhs.id;
    }

    node["type"] = rhs.type;

    if (!rhs.props.empty())
    {
      node["props"] = rhs.props;
    }

    if (!rhs.layout.empty())
    {
      node["layout"] = rhs.layout;
    }

    if (!rhs.children.empty())
    {
      node["children"] = rhs.children;
    }

    return node;
  }

  bool convert<LayoutNode>::decode(Node const& node, LayoutNode& rhs)
  {
    if (!node.IsMap())
    {
      return false;
    }

    if (node["id"])
    {
      rhs.id = node["id"].as<std::string>();
    }

    if (node["type"])
    {
      rhs.type = node["type"].as<std::string>();
    }

    if (node["props"])
    {
      rhs.props = node["props"].as<std::map<std::string, LayoutValue, std::less<>>>();
    }

    if (node["layout"])
    {
      rhs.layout = node["layout"].as<std::map<std::string, LayoutValue, std::less<>>>();
    }

    if (node["children"])
    {
      rhs.children = node["children"].as<std::vector<LayoutNode>>();
    }

    return true;
  }

  Node convert<LayoutDocument>::encode(LayoutDocument const& rhs)
  {
    auto node = Node{};
    node["version"] = static_cast<int>(rhs.version);
    node["root"] = rhs.root;

    if (!rhs.templates.empty())
    {
      node["templates"] = rhs.templates;
    }

    return node;
  }

  bool convert<LayoutDocument>::decode(Node const& node, LayoutDocument& rhs)
  {
    if (!node.IsMap() || !node["version"] || !node["root"])
    {
      return false;
    }

    rhs.version = node["version"].as<int>();
    rhs.root = node["root"].as<LayoutNode>();

    if (node["templates"])
    {
      rhs.templates = node["templates"].as<std::map<std::string, LayoutNode, std::less<>>>();
    }

    return true;
  }
} // namespace YAML

namespace ao::gtk::layout
{
  namespace
  {
    LayoutDocument loadBuiltInLayout()
    {
      try
      {
        auto const bytes = Gio::Resource::lookup_data_global("/org/aobus/layout/default_layout.yaml");
        gsize size = 0; // NOLINT(misc-include-cleaner)
        auto const* const data = static_cast<char const*>(bytes->get_data(size));
        auto const yamlStr = std::string_view{data, size};
        auto const node = YAML::Load(std::string{yamlStr});
        return node.as<LayoutDocument>();
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_CRITICAL("LayoutDocument: GResource error: {}", e.what());
        throw;
      }
    }
  } // namespace

  LayoutDocument createDefaultLayout()
  {
    return loadBuiltInLayout();
  }

  std::map<std::string, LayoutNode, std::less<>> getBuiltInTemplates()
  {
    return loadBuiltInLayout().templates;
  }
} // namespace ao::gtk::layout
