// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/utility/Fnv1a.h>
#include <ao/yaml/ConfigTraits.h>
#include <ao/yaml/Utils.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::uimodel
{
  namespace
  {
    std::string canonicalDouble(double value)
    {
      auto buffer = std::array<char, 64>{};
      auto const result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);

      if (result.ec == std::errc{})
      {
        return {buffer.data(), result.ptr};
      }

      auto stream = std::ostringstream{};
      stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
      return stream.str();
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

    std::string propString(LayoutNode const& node, std::string_view key, std::string const& defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asString(defaultValue);
    }

    std::int64_t propInt(LayoutNode const& node, std::string_view key, std::int64_t defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asInt(defaultValue);
    }

    double propDouble(LayoutNode const& node, std::string_view key, double defaultValue)
    {
      auto const* const value = findProp(node, key);
      return value == nullptr ? defaultValue : value->asDouble(defaultValue);
    }

    bool propBool(LayoutNode const& node, std::string_view key, bool defaultValue)
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
        appendField(canonical, "orientation", propString(node, "orientation", "vertical"));
        appendDoubleField(canonical, "initialPositionPercent", propDouble(node, "initialPositionPercent", 0.0));
        appendIntField(canonical, "position", propInt(node, "position", -1));
        appendBoolField(canonical, "resizeStart", propBool(node, "resizeStart", true));
        appendBoolField(canonical, "resizeEnd", propBool(node, "resizeEnd", true));
        appendBoolField(canonical, "shrinkStart", propBool(node, "shrinkStart", false));
        appendBoolField(canonical, "shrinkEnd", propBool(node, "shrinkEnd", false));
        return canonical;
      }

      if (node.type == kCollapsibleSplitComponentType)
      {
        appendField(canonical, "orientation", propString(node, "orientation", "horizontal"));
        appendField(canonical, "collapseSide", propString(node, "collapseSide", "end"));
        appendDoubleField(canonical, "initialPositionPercent", propDouble(node, "initialPositionPercent", 0.0));
        appendIntField(canonical, "position", propInt(node, "position", -1));
        appendBoolField(canonical, "revealed", propBool(node, "revealed", true));
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
    return utility::fnv1a64Hex(canonicalBaseline(node));
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
} // namespace ao::uimodel

namespace ao::yaml
{
  using namespace ao::uimodel;

  void write(ryml::NodeRef node, LayoutComponentStateEntry const& value)
  {
    node |= ryml::MAP;
    write(node.append_child() << ryml::key("type"), value.type);
    write(node.append_child() << ryml::key("stateVersion"), value.stateVersion);
    write(node.append_child() << ryml::key("baselineHash"), value.baselineHash);
    write(node.append_child() << ryml::key("state"), value.state);
  }

  bool read(ryml::ConstNodeRef node, LayoutComponentStateEntry& value)
  {
    if (!node.is_map())
    {
      return false;
    }

    auto const typeNode = findChild(node, "type");
    auto const stateVersionNode = findChild(node, "stateVersion");
    auto const baselineHashNode = findChild(node, "baselineHash");
    auto const stateNode = findChild(node, "state");

    if (!typeNode.readable() || !stateVersionNode.readable() || !baselineHashNode.readable() || !stateNode.readable())
    {
      return false;
    }

    return read(typeNode, value.type) && read(stateVersionNode, value.stateVersion) &&
           read(baselineHashNode, value.baselineHash) && read(stateNode, value.state);
  }

  void write(ryml::NodeRef node, LayoutComponentStateDocument const& value)
  {
    node |= ryml::MAP;
    write(node.append_child() << ryml::key("version"), value.version);
    write(node.append_child() << ryml::key("preset"), value.preset);
    write(node.append_child() << ryml::key("components"), value.components);
  }

  bool read(ryml::ConstNodeRef node, LayoutComponentStateDocument& value)
  {
    if (!node.is_map())
    {
      return false;
    }

    auto const versionNode = findChild(node, "version");
    auto const presetNode = findChild(node, "preset");
    auto const componentsNode = findChild(node, "components");

    if (!versionNode.readable() || !presetNode.readable() || !componentsNode.readable())
    {
      return false;
    }

    return read(versionNode, value.version) && read(presetNode, value.preset) && read(componentsNode, value.components);
  }
} // namespace ao::yaml
