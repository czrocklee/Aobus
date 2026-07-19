// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  Result<> writeLayoutValueMap(ryml::NodeRef node, LayoutValueMap const& values)
  {
    return yaml::writeStringMap(node, values, "layout value map", writeLayoutValue);
  }

  Result<LayoutValueMap> readLayoutValueMap(ryml::ConstNodeRef node, std::string_view context)
  {
    return yaml::readStringMap<LayoutValueMap>(node, context, readLayoutValue);
  }

  namespace
  {
    using LayoutNodeMap = std::map<std::string, LayoutNode, std::less<>>;

    Result<> writeLayoutNodeMap(ryml::NodeRef node, LayoutNodeMap const& values)
    {
      return yaml::writeStringMap(node, values, "layout templates", writeLayoutNode);
    }

    Result<LayoutNodeMap> readLayoutNodeMap(ryml::ConstNodeRef node, std::string_view context)
    {
      return yaml::readStringMap<LayoutNodeMap>(node, context, readLayoutNode);
    }
  } // namespace

  Result<> writeLayoutValue(ryml::NodeRef node, LayoutValue const& value)
  {
    return std::visit(
      [&node](auto const& nodeValue) -> Result<>
      {
        using T = std::decay_t<decltype(nodeValue)>;

        if constexpr (std::is_same_v<T, std::monostate>)
        {
          node << nullptr;
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        {
          return yaml::writeScalarSequence(node, nodeValue);
        }
        else
        {
          yaml::writeScalar(node, nodeValue);
        }

        return {};
      },
      value.data);
  }

  Result<LayoutValue> readLayoutValue(ryml::ConstNodeRef node, std::string_view context)
  {
    if (node.invalid() || (node.has_val() && node.val_is_null()))
    {
      return LayoutValue{std::monostate{}};
    }

    if (node.has_val())
    {
      auto const scalar = yaml::scalarView(node);

      if (node.is_val_quoted())
      {
        return LayoutValue{std::string{scalar}};
      }

      if (scalar == "true")
      {
        return LayoutValue{true};
      }

      if (scalar == "false")
      {
        return LayoutValue{false};
      }

      if (std::int64_t integer = 0; yaml::tryParseScalar(scalar, integer))
      {
        return LayoutValue{integer};
      }

      if (double real = 0.0; yaml::tryParseScalar(scalar, real))
      {
        return LayoutValue{real};
      }

      return LayoutValue{std::string{scalar}};
    }

    if (node.is_seq())
    {
      auto sequence = yaml::readScalarSequence<std::string>(node, context);

      if (!sequence)
      {
        return std::unexpected{sequence.error()};
      }

      return LayoutValue{std::move(*sequence)};
    }

    return makeError(Error::Code::FormatRejected,
                     yaml::boundedErrorContext(context) + " must be null, a scalar, or a scalar sequence");
  }

  Result<> writeLayoutNode(ryml::NodeRef node, LayoutNode const& value)
  {
    if (value.type.empty())
    {
      return makeError(Error::Code::InvalidState, "Layout node type must not be empty");
    }

    auto writer = yaml::MapWriter{node};

    if (!value.id.empty())
    {
      writer.scalar("id", value.id);
    }

    writer.scalar("type", value.type);

    if (!value.props.empty())
    {
      writer.value("props", value.props, writeLayoutValueMap);
    }

    if (!value.layout.empty())
    {
      writer.value("layout", value.layout, writeLayoutValueMap);
    }

    if (!value.children.empty())
    {
      writer.sequence("children", value.children, writeLayoutNode);
    }

    if (value.optTooltip && value.optTooltip->nodePtr)
    {
      writer.value("tooltip", *value.optTooltip->nodePtr, writeLayoutNode);
    }

    return std::move(writer).finish();
  }

  Result<LayoutNode> readLayoutNode(ryml::ConstNodeRef node, std::string_view context)
  {
    constexpr auto kKeys = std::to_array<std::string_view>({"id", "type", "props", "layout", "children", "tooltip"});

    auto value = LayoutNode{};
    auto reader = yaml::MapReader{node, kKeys, context};
    reader.requiredScalar("type", value.type)
      .optionalScalar("id", value.id)
      .optionalValue("props", value.props, readLayoutValueMap)
      .optionalValue("layout", value.layout, readLayoutValueMap)
      .optionalSequence("children", value.children, readLayoutNode);

    if (!reader.result())
    {
      return std::unexpected{reader.result().error()};
    }

    if (value.type.empty())
    {
      return makeError(Error::Code::FormatRejected, yaml::fieldContext(context, "type") + " must not be empty");
    }

    if (auto const tooltipNode = yaml::findChild(node, "tooltip"); tooltipNode.readable())
    {
      auto tooltip = readLayoutNode(tooltipNode, yaml::fieldContext(context, "tooltip"));

      if (!tooltip)
      {
        return std::unexpected{tooltip.error()};
      }

      value.optTooltip = BoxedLayoutNode{std::move(*tooltip)};
    }

    return value;
  }

  Result<> LayoutDocumentYamlSchema::serialize(ryml::NodeRef node, LayoutDocument const& document) const
  {
    if (document.version != kLayoutDocumentVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported layout document version {}", document.version));
    }

    auto writer = yaml::MapWriter{node};
    writer.scalar("version", document.version).value("root", document.root, writeLayoutNode);

    if (!document.templates.empty())
    {
      writer.value("templates", document.templates, writeLayoutNodeMap);
    }

    return std::move(writer).finish();
  }

  Result<LayoutDocument> LayoutDocumentYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                               LayoutDocument const& /*seed*/) const
  {
    constexpr auto kContext = std::string_view{"layout document"};

    if (auto const result = yaml::requireMap(node, kContext); !result)
    {
      return std::unexpected{result.error()};
    }

    auto version = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

    if (!version)
    {
      return std::unexpected{version.error()};
    }

    if (*version != kLayoutDocumentVersion)
    {
      return makeError(Error::Code::NotSupported, std::format("Unsupported layout document version {}", *version));
    }

    constexpr auto kKeys = std::to_array<std::string_view>({"version", "root", "templates"});

    auto document = LayoutDocument{.version = *version};
    auto reader = yaml::MapReader{node, kKeys, kContext};
    reader.requiredValue("root", document.root, readLayoutNode)
      .optionalValue("templates", document.templates, readLayoutNodeMap);
    return std::move(reader).finish(std::move(document));
  }

  Result<bool> loadLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument& doc)
  {
    return store.load(group, doc, LayoutDocumentYamlSchema{});
  }

  Result<> saveLayout(rt::ConfigStore& store, std::string_view group, LayoutDocument const& doc)
  {
    return store.save(group, doc, LayoutDocumentYamlSchema{});
  }
} // namespace ao::uimodel
