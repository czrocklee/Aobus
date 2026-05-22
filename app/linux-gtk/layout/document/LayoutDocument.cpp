// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"

#include "ao/Error.h"
#include "ao/utility/Log.h"
#include "layout/document/LayoutNode.h"
#include "layout/document/LayoutYaml.h"
#include "runtime/ConfigStore.h"
#include "runtime/yaml/ConfigTraits.h" // NOLINT(misc-include-cleaner)
#include "runtime/yaml/Utils.h"

#include <giomm/resource.h>
#include <glib.h>
#include <glibmm/error.h>

#include <charconv>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::yaml
{
  using namespace ao::gtk::layout;

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
            yaml::setValue(node.append_child(), item);
          }
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
          yaml::setValue(node, nodeValue);
        }
        else
        {
          node << nodeValue;
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
      auto const scalar = yaml::scalarView(node);

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
          sequence.emplace_back(yaml::scalarView(item));
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
  }

  bool read(ryml::ConstNodeRef node, LayoutNode& value)
  {
    if (!node.is_map())
    {
      return false;
    }

    if (auto const idNode = yaml::findChild(node, "id"); idNode.readable())
    {
      value.id = yaml::scalarView(idNode);
    }

    if (auto const typeNode = yaml::findChild(node, "type"); typeNode.readable())
    {
      value.type = yaml::scalarView(typeNode);
    }

    if (auto const propsNode = yaml::findChild(node, "props"); propsNode.readable())
    {
      read(propsNode, value.props);
    }

    if (auto const layoutNode = yaml::findChild(node, "layout"); layoutNode.readable())
    {
      read(layoutNode, value.layout);
    }

    if (auto const childrenNode = yaml::findChild(node, "children"); childrenNode.readable())
    {
      read(childrenNode, value.children);
    }

    return true;
  }

  void write(ryml::NodeRef node, LayoutDocument const& value)
  {
    node |= ryml::MAP;
    node.append_child() << ryml::key("version") << static_cast<int>(value.version);
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

    auto const versionNode = yaml::findChild(node, "version");
    auto const rootNode = yaml::findChild(node, "root");

    if (!versionNode.readable() || !rootNode.readable())
    {
      return false;
    }

    read(versionNode, value.version);
    read(rootNode, value.root);

    if (auto const templatesNode = yaml::findChild(node, "templates"); templatesNode.readable())
    {
      read(templatesNode, value.templates);
    }

    return true;
  }
} // namespace ao::rt::yaml

namespace ao::gtk::layout
{
  namespace
  {
    LayoutDocument loadBuiltInLayout()
    {
      try
      {
        auto const bytes = Gio::Resource::lookup_data_global("/org/aobus/layout/default_layout.yaml");
        gsize size = 0;
        auto const* const data = static_cast<char const*>(bytes->get_data(size));

        auto tree = ryml::Tree{rt::yaml::callbacks("default_layout.yaml")};
        ryml::parse_in_arena(rt::yaml::toCsubstr(std::string_view{data, size}), &tree);

        auto doc = LayoutDocument{};

        if (!rt::yaml::read(tree.rootref(), doc))
        {
          APP_LOG_CRITICAL("LayoutDocument: Failed to decode built-in layout");
          throw std::runtime_error{"Failed to decode built-in layout"};
        }

        return doc;
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

  Result<> loadLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument& doc)
  {
    return store.load(group, doc);
  }

  void saveLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument const& doc)
  {
    store.save(group, doc);
  }
} // namespace ao::gtk::layout
