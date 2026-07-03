// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// clang-format off
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
// clang-format on

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/yaml/ConfigTraits.h>
#include <ao/yaml/Utils.h>

#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::yaml
{
  using namespace ao::uimodel;

  void write(ryml::NodeRef node, LayoutValue const& value)
  {
    std::visit(
      [&node](auto const& nodeValue)
      {
        using T = std::decay_t<decltype(nodeValue)>;

        if constexpr (std::is_same_v<T, std::monostate>)
        {
          node << nullptr;
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        {
          node |= ryml::SEQ;

          for (auto const& item : nodeValue)
          {
            setValue(node.append_child(), item);
          }
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
          setValue(node, nodeValue);
        }
        else
        {
          write(node, nodeValue);
        }
      },
      value.data);
  }

  bool read(ryml::ConstNodeRef node, LayoutValue& value)
  {
    if (node.invalid() || (node.has_val() && node.val_is_null()))
    {
      value.data = std::monostate{};
      return true;
    }

    if (node.has_val())
    {
      auto const scalar = scalarView(node);

      if (scalar == "true")
      {
        value.data = true;
        return true;
      }

      if (scalar == "false")
      {
        value.data = false;
        return true;
      }

      auto const* const first = scalar.data();
      auto const* const last = first + scalar.size();

      {
        std::int64_t intValue = 0;

        if (auto const intResult = std::from_chars(first, last, intValue);
            intResult.ec == std::errc{} && intResult.ptr == last)
        {
          value.data = intValue;
          return true;
        }
      }

      {
        double doubleValue = 0.0;

        if (auto const doubleResult = std::from_chars(first, last, doubleValue);
            doubleResult.ec == std::errc{} && doubleResult.ptr == last)
        {
          value.data = doubleValue;
          return true;
        }

        value.data = std::string{scalar};
        return true;
      }
    }

    if (node.is_seq())
    {
      auto sequence = std::vector<std::string>{};

      for (auto const& item : node.children())
      {
        if (item.has_val())
        {
          sequence.emplace_back(scalarView(item));
        }
      }

      value.data = std::move(sequence);
      return true;
    }

    return false;
  }

  void write(ryml::NodeRef node, LayoutNode const& value)
  {
    node |= ryml::MAP;

    if (!value.id.empty())
    {
      node.append_child() << ryml::key("id") << value.id;
    }

    node.append_child() << ryml::key("type") << value.type;

    if (!value.props.empty())
    {
      auto child = node.append_child();
      child << ryml::key("props");
      write(child, value.props);
    }

    if (!value.layout.empty())
    {
      auto child = node.append_child();
      child << ryml::key("layout");
      write(child, value.layout);
    }

    if (!value.children.empty())
    {
      auto child = node.append_child();
      child << ryml::key("children");
      write(child, value.children);
    }

    if (value.optTooltip && value.optTooltip->nodePtr)
    {
      auto child = node.append_child();
      child << ryml::key("tooltip");
      write(child, *value.optTooltip->nodePtr);
    }
  }

  bool read(ryml::ConstNodeRef node, LayoutNode& value)
  {
    if (!node.is_map())
    {
      return false;
    }

    if (auto const idNode = findChild(node, "id"); idNode.readable())
    {
      value.id = scalarView(idNode);
    }

    if (auto const typeNode = findChild(node, "type"); typeNode.readable())
    {
      value.type = scalarView(typeNode);
    }

    if (auto const propsNode = findChild(node, "props"); propsNode.readable())
    {
      read(propsNode, value.props);
    }

    if (auto const layoutNode = findChild(node, "layout"); layoutNode.readable())
    {
      read(layoutNode, value.layout);
    }

    if (auto const childrenNode = findChild(node, "children"); childrenNode.readable())
    {
      read(childrenNode, value.children);
    }

    if (auto const tooltipNode = findChild(node, "tooltip"); tooltipNode.readable())
    {
      if (auto tooltipValue = LayoutNode{}; read(tooltipNode, tooltipValue))
      {
        value.optTooltip = BoxedLayoutNode{std::move(tooltipValue)};
      }
    }

    return true;
  }

  void write(ryml::NodeRef node, LayoutDocument const& value)
  {
    node |= ryml::MAP;
    node.append_child() << ryml::key("version") << static_cast<std::int32_t>(value.version);
    write(node.append_child() << ryml::key("root"), value.root);

    if (!value.templates.empty())
    {
      auto child = node.append_child();
      child << ryml::key("templates");
      write(child, value.templates);
    }
  }

  bool read(ryml::ConstNodeRef node, LayoutDocument& value)
  {
    if (!node.is_map())
    {
      return false;
    }

    auto const versionNode = findChild(node, "version");
    auto const rootNode = findChild(node, "root");

    if (!versionNode.readable() || !rootNode.readable())
    {
      return false;
    }

    read(versionNode, value.version);
    read(rootNode, value.root);

    if (auto const templatesNode = findChild(node, "templates"); templatesNode.readable())
    {
      read(templatesNode, value.templates);
    }

    return true;
  }
} // namespace ao::yaml

namespace ao::uimodel
{
  Result<> loadLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument& doc)
  {
    return store.load(group, doc);
  }

  void saveLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument const& doc)
  {
    store.save(group, doc);
  }
} // namespace ao::uimodel
