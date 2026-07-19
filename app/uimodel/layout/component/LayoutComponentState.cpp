// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/utility/Xxh3.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    Result<> writeEntry(ryml::NodeRef node, LayoutComponentStateEntry const& entry)
    {
      if (entry.type.empty() || entry.baselineHash.empty())
      {
        return makeError(Error::Code::InvalidState, "Layout component state type and baseline hash must not be empty");
      }

      if (entry.stateVersion != kStateEntryVersion)
      {
        return makeError(Error::Code::NotSupported,
                         std::format("Unsupported layout component state entry version {}", entry.stateVersion));
      }

      auto writer = yaml::MapWriter{node};
      writer.scalar("type", entry.type)
        .scalar("stateVersion", entry.stateVersion)
        .scalar("baselineHash", entry.baselineHash)
        .value("state", entry.state, writeLayoutValueMap);
      return std::move(writer).finish();
    }

    Result<LayoutComponentStateEntry> readEntry(ryml::ConstNodeRef node, std::string_view context)
    {
      if (auto const result = yaml::requireMap(node, context); !result)
      {
        return std::unexpected{result.error()};
      }

      auto stateVersion = yaml::requireScalar<std::uint32_t>(node, "stateVersion", context);

      if (!stateVersion)
      {
        return std::unexpected{stateVersion.error()};
      }

      if (*stateVersion != kStateEntryVersion)
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported layout component state entry version {}", *stateVersion));
      }

      constexpr auto kKeys = std::to_array<std::string_view>({"type", "stateVersion", "baselineHash", "state"});

      auto entry = LayoutComponentStateEntry{.stateVersion = *stateVersion};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("type", entry.type)
        .requiredScalar("baselineHash", entry.baselineHash)
        .requiredValue("state", entry.state, readLayoutValueMap);

      if (!reader.result())
      {
        return std::unexpected{reader.result().error()};
      }

      if (entry.type.empty() || entry.baselineHash.empty())
      {
        return makeError(
          Error::Code::FormatRejected, yaml::boundedErrorContext(context) + " type and baselineHash must not be empty");
      }

      return entry;
    }

    using ComponentStateMap = std::map<std::string, LayoutComponentStateEntry, std::less<>>;

    Result<> writeComponents(ryml::NodeRef node, ComponentStateMap const& components)
    {
      return yaml::writeStringMap(node, components, "layout component state components", writeEntry);
    }

    Result<ComponentStateMap> readComponents(ryml::ConstNodeRef node, std::string_view context)
    {
      return yaml::readStringMap<ComponentStateMap>(node, context, readEntry);
    }

    std::string canonicalDouble(double value)
    {
      auto buffer = std::array<char, 64>{};
      auto const result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);

      if (result.ec == std::errc{})
      {
        return {buffer.data(), result.ptr};
      }

      return std::format("{:.17g}", value);
    }

    void appendField(std::string& canonical, std::string_view key, std::string_view value)
    {
      canonical.append(key);
      canonical.push_back('=');
      canonical.append(value);
      canonical.push_back('\n');
    }

    void appendBoolField(std::string& canonical, std::string_view key, bool value)
    {
      appendField(canonical, key, value ? "true" : "false");
    }

    void appendIntField(std::string& canonical, std::string_view key, std::int64_t value)
    {
      appendField(canonical, key, std::to_string(value));
    }

    void appendDoubleField(std::string& canonical, std::string_view key, double value)
    {
      appendField(canonical, key, canonicalDouble(value));
    }

    LayoutValue const* findProp(LayoutNode const& node, std::string_view key)
    {
      auto const it = node.props.find(key);
      return it == node.props.end() ? nullptr : &it->second;
    }

    std::string stringPropertyOr(LayoutNode const& node, std::string_view key, std::string const& defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asString(defaultValue);
    }

    std::int64_t integerPropertyOr(LayoutNode const& node, std::string_view key, std::int64_t defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asInt(defaultValue);
    }

    double doublePropertyOr(LayoutNode const& node, std::string_view key, double defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asDouble(defaultValue);
    }

    bool booleanPropertyOr(LayoutNode const& node, std::string_view key, bool defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asBool(defaultValue);
    }

    std::string canonicalBaseline(LayoutNode const& node)
    {
      auto canonical = std::string{};
      appendField(canonical, "type", node.type);

      if (node.type == kSplitComponentType)
      {
        appendField(canonical, "orientation", stringPropertyOr(node, "orientation", "vertical"));
        appendDoubleField(canonical, "initialPositionPercent", doublePropertyOr(node, "initialPositionPercent", 0.0));
        appendIntField(canonical, "position", integerPropertyOr(node, "position", -1));
        appendBoolField(canonical, "resizeStart", booleanPropertyOr(node, "resizeStart", true));
        appendBoolField(canonical, "resizeEnd", booleanPropertyOr(node, "resizeEnd", true));
        appendBoolField(canonical, "shrinkStart", booleanPropertyOr(node, "shrinkStart", false));
        appendBoolField(canonical, "shrinkEnd", booleanPropertyOr(node, "shrinkEnd", false));
        return canonical;
      }

      if (node.type == kCollapsibleSplitComponentType)
      {
        appendField(canonical, "orientation", stringPropertyOr(node, "orientation", "horizontal"));
        appendField(canonical, "collapseSide", stringPropertyOr(node, "collapseSide", "end"));
        appendDoubleField(canonical, "initialPositionPercent", doublePropertyOr(node, "initialPositionPercent", 0.0));
        appendIntField(canonical, "position", integerPropertyOr(node, "position", -1));
        appendBoolField(canonical, "revealed", booleanPropertyOr(node, "revealed", true));
      }

      return canonical;
    }

    std::map<std::string, LayoutNode, std::less<>> statefulNodeIndex(LayoutDocument const& doc)
    {
      auto result = std::map<std::string, LayoutNode, std::less<>>{};

      visitExpandedLayoutNodes(doc,
                               [&result](LayoutNode const& node)
                               {
                                 if (!node.id.empty() && isStatefulLayoutComponentType(node.type))
                                 {
                                   result.emplace(node.id, node);
                                 }
                               });

      return result;
    }
  } // namespace

  std::string componentBaselineHash(LayoutNode const& node)
  {
    return utility::xxh3Hash64Hex(canonicalBaseline(node));
  }

  std::optional<LayoutComponentStateEntry> resolveComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                 std::string_view componentId,
                                                                 std::string_view componentType,
                                                                 std::string_view baselineHash)
  {
    if (stateDoc.version != kStateFileVersion || componentId.empty())
    {
      return std::nullopt;
    }

    auto const it = stateDoc.components.find(componentId);

    if (it == stateDoc.components.end())
    {
      return std::nullopt;
    }

    auto const& entry = it->second;

    if (entry.type != componentType || entry.stateVersion != kStateEntryVersion || entry.baselineHash != baselineHash)
    {
      return std::nullopt;
    }

    return entry;
  }

  std::optional<LayoutComponentStateEntry> resolveComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                 LayoutNode const& node)
  {
    return resolveComponentState(stateDoc, node.id, node.type, componentBaselineHash(node));
  }

  void pruneComponentState(LayoutComponentStateDocument& stateDoc, LayoutDocument const& effectiveDoc)
  {
    if (stateDoc.version != kStateFileVersion)
    {
      stateDoc.components.clear();
      return;
    }

    auto const nodesById = statefulNodeIndex(effectiveDoc);

    std::erase_if(stateDoc.components,
                  [&nodesById](auto const& item)
                  {
                    auto const& [componentId, entry] = item;
                    auto const nodeIt = nodesById.find(componentId);

                    if (nodeIt == nodesById.end())
                    {
                      return true;
                    }

                    auto const& node = nodeIt->second;
                    return entry.type != node.type || entry.stateVersion != kStateEntryVersion ||
                           entry.baselineHash != componentBaselineHash(node);
                  });
  }

  Result<> LayoutComponentStateYamlSchema::serialize(ryml::NodeRef node,
                                                     LayoutComponentStateDocument const& document) const
  {
    if (document.version != kStateFileVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported layout component state version {}", document.version));
    }

    if (document.preset.empty())
    {
      return makeError(Error::Code::InvalidState, "Layout component state preset id must not be empty");
    }

    auto writer = yaml::MapWriter{node};
    writer.scalar("version", document.version)
      .scalar("preset", document.preset)
      .value("components", document.components, writeComponents);
    return std::move(writer).finish();
  }

  Result<LayoutComponentStateDocument> LayoutComponentStateYamlSchema::deserialize(
    ryml::ConstNodeRef node,
    LayoutComponentStateDocument const& /*seed*/) const
  {
    constexpr auto kContext = std::string_view{"layout component state"};

    if (auto const result = yaml::requireMap(node, kContext); !result)
    {
      return std::unexpected{result.error()};
    }

    auto version = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

    if (!version)
    {
      return std::unexpected{version.error()};
    }

    if (*version != kStateFileVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported layout component state version {}", *version));
    }

    constexpr auto kKeys = std::to_array<std::string_view>({"version", "preset", "components"});

    auto document = LayoutComponentStateDocument{.version = *version};
    auto reader = yaml::MapReader{node, kKeys, kContext};
    reader.requiredScalar("preset", document.preset).requiredValue("components", document.components, readComponents);

    if (!reader.result())
    {
      return std::unexpected{reader.result().error()};
    }

    if (document.preset.empty())
    {
      return makeError(Error::Code::FormatRejected, "Layout component state preset id must not be empty");
    }

    return document;
  }
} // namespace ao::uimodel
