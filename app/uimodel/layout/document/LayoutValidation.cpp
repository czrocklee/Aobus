// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutValidation.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutDialect.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPlacement.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    using NodeIdSet =
      boost::unordered_flat_set<std::string, utility::TransparentStringHash, utility::TransparentStringEqual>;

    constexpr auto kAllSlots = std::to_array({ActionSlot::PrimaryClick,
                                              ActionSlot::PrimaryLongPress,
                                              ActionSlot::SecondaryClick,
                                              ActionSlot::SecondaryLongPress});

    std::string nodeLabel(LayoutNode const& node)
    {
      return node.id.empty() ? node.type : node.id;
    }

    LayoutRejection reject(LayoutRejectionReason const reason,
                           LayoutNode const& node,
                           std::string detail,
                           std::string message)
    {
      return {.reason = reason, .nodeId = nodeLabel(node), .detail = std::move(detail), .message = std::move(message)};
    }

    bool matchesPropertyKind(PropertySchema const& propertySchema, LayoutValue const& value);

    PropertySchema const* findLayoutProperty(ComponentSchema const& schema, std::string const& name)
    {
      auto const it = std::ranges::find(schema.layoutProperties, name, &PropertySchema::name);
      return it == schema.layoutProperties.end() ? nullptr : &*it;
    }

    std::optional<LayoutRejection> validateLayoutField(LayoutNode const& node,
                                                       ComponentSchema const& componentSchema,
                                                       ComponentSchema const* const parentSchema,
                                                       LayoutDialect const& dialect,
                                                       std::string const& name,
                                                       LayoutValue const& value)
    {
      // The dialect rules first: a frontend rejects another frontend's styling
      // field even where a component schema would have accepted it.
      if (dialect.layoutField != nullptr)
      {
        auto verdict = dialect.layoutField(node, name, value);

        if (verdict.ruling == LayoutFieldRuling::Rejected)
        {
          return reject(verdict.reason, node, name, std::move(verdict.message));
        }

        if (verdict.ruling == LayoutFieldRuling::Accepted)
        {
          return std::nullopt;
        }
      }

      if ((name == "halign" || name == "valign") && !layoutAlignmentFromString(value.asString()))
      {
        return reject(LayoutRejectionReason::InvalidLayoutFieldValue,
                      node,
                      name,
                      std::format("Unsupported alignment '{}'", value.asString()));
      }

      if (name == "widthRequest" || name == "heightRequest")
      {
        if (!value.isNumber())
        {
          return reject(LayoutRejectionReason::InvalidLayoutFieldValue, node, name, "A size request must be a number");
        }

        auto const size = value.asDouble();

        if (!std::isfinite(size))
        {
          return reject(LayoutRejectionReason::InvalidLayoutFieldValue, node, name, "A size request must be finite");
        }

        if (size > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        {
          return reject(
            LayoutRejectionReason::InvalidLayoutFieldValue, node, name, "A size request exceeds the frontend range");
        }
      }

      if (isCommonLayoutProp(name))
      {
        return std::nullopt;
      }

      // A container may declare layout fields that are authored on each of
      // its direct children (for example, CenterBox's `slot`). The child is
      // still validated against its own schema first, but the parent is
      // the owner of these placement fields and must get a chance to accept
      // them as well.
      auto const* layoutProp = findLayoutProperty(componentSchema, name);

      if (layoutProp == nullptr && parentSchema != nullptr)
      {
        layoutProp = findLayoutProperty(*parentSchema, name);
      }

      if (layoutProp != nullptr)
      {
        if (!matchesPropertyKind(*layoutProp, value))
        {
          return reject(LayoutRejectionReason::InvalidLayoutFieldValue,
                        node,
                        name,
                        std::format("Layout field '{}' has the wrong value type", name));
        }

        if (layoutProp->kind == PropertyKind::Enum && !layoutProp->enumValues.empty() &&
            !std::ranges::contains(layoutProp->enumValues, value.asString()))
        {
          return reject(LayoutRejectionReason::InvalidLayoutFieldValue,
                        node,
                        name,
                        std::format("Layout field '{}' does not accept '{}'", name, value.asString()));
        }

        return std::nullopt;
      }

      return reject(LayoutRejectionReason::UnsupportedLayoutField,
                    node,
                    name,
                    std::format("Component '{}' does not accept the layout field", node.type));
    }

    bool matchesPropertyKind(PropertySchema const& propertySchema, LayoutValue const& value)
    {
      switch (propertySchema.kind)
      {
        case PropertyKind::Bool: return value.getIf<bool>() != nullptr;
        case PropertyKind::Int: return value.getIf<std::int64_t>() != nullptr;
        case PropertyKind::Double:
        case PropertyKind::Size: return value.isNumber();
        case PropertyKind::String:
        case PropertyKind::Enum: return value.getIf<std::string>() != nullptr;
        case PropertyKind::StringList:
          return value.getIf<std::vector<std::string>>() != nullptr || value.getIf<std::string>() != nullptr;
      }

      return false;
    }

    bool isGlobalActionProp(std::string_view const name)
    {
      return std::ranges::any_of(
        kAllSlots, [name](ActionSlot const slot) { return LayoutSchema::actionProperty(slot) == name; });
    }

    std::optional<LayoutRejection> validateProperty(LayoutNode const& node,
                                                    ComponentSchema const& componentSchema,
                                                    std::string const& name,
                                                    LayoutValue const& value)
    {
      if (isGlobalActionProp(name))
      {
        // Action slots carry their own policy and schema rules.
        return std::nullopt;
      }

      auto const it = std::ranges::find(componentSchema.properties, name, &PropertySchema::name);

      if (it == componentSchema.properties.end())
      {
        return reject(LayoutRejectionReason::UnknownProperty,
                      node,
                      name,
                      std::format("Component '{}' does not declare the property", node.type));
      }

      if (!matchesPropertyKind(*it, value))
      {
        return reject(LayoutRejectionReason::InvalidPropertyValue,
                      node,
                      name,
                      std::format("Property '{}' has the wrong value type", name));
      }

      // Action properties are injected with an open enum kind; their allowed
      // values are the action schema rather than a fixed value list.
      if (it->kind == PropertyKind::Enum && !it->enumValues.empty() &&
          !std::ranges::contains(it->enumValues, value.asString()))
      {
        return reject(LayoutRejectionReason::InvalidPropertyValue,
                      node,
                      name,
                      std::format("Property '{}' does not accept '{}'", name, value.asString()));
      }

      return std::nullopt;
    }

    std::optional<LayoutRejection> validateChildCount(LayoutNode const& node,
                                                      ComponentSchema const& componentSchema,
                                                      LayoutDialect const& dialect)
    {
      auto const count = node.children.size();

      if (dialect.presentationChildCount != nullptr)
      {
        if (auto const optRequired = dialect.presentationChildCount(node); optRequired && count != *optRequired)
        {
          auto const reason = count < *optRequired ? LayoutRejectionReason::ChildCountBelowMinimum
                                                   : LayoutRejectionReason::ChildCountAboveMaximum;
          return reject(
            reason,
            node,
            std::string{"children"},
            std::format("The authored presentation requires exactly {} children, found {}", *optRequired, count));
        }
      }

      if (count < componentSchema.minChildren)
      {
        return reject(
          LayoutRejectionReason::ChildCountBelowMinimum,
          node,
          std::string{"children"},
          std::format(
            "Component '{}' requires at least {} children, found {}", node.type, componentSchema.minChildren, count));
      }

      if (componentSchema.optMaxChildren && count > *componentSchema.optMaxChildren)
      {
        return reject(
          LayoutRejectionReason::ChildCountAboveMaximum,
          node,
          std::string{"children"},
          std::format(
            "Component '{}' accepts at most {} children, found {}", node.type, *componentSchema.optMaxChildren, count));
      }

      return std::nullopt;
    }

    std::optional<LayoutRejection> validateActions(LayoutNode const& node,
                                                   ComponentSchema const& componentSchema,
                                                   LayoutSchema const& schema)
    {
      for (auto const slot : kAllSlots)
      {
        auto const propName = LayoutSchema::actionProperty(slot);
        auto const it = node.props.find(propName);

        if (it == node.props.end())
        {
          continue;
        }

        if (!componentSchema.allows(slot))
        {
          return reject(LayoutRejectionReason::UnsupportedActionSlot,
                        node,
                        std::string{propName},
                        std::format("Component '{}' does not support the action slot", node.type));
        }

        auto const* const boundId = it->second.getIf<std::string>();

        if (boundId == nullptr)
        {
          return reject(LayoutRejectionReason::InvalidPropertyValue,
                        node,
                        std::string{propName},
                        "An action binding must be an action id string");
        }

        auto const& actionId = *boundId;

        if (actionId.empty() || actionId == "none")
        {
          continue;
        }

        if (!schema.action(actionId))
        {
          return reject(
            LayoutRejectionReason::UnknownAction, node, actionId, std::format("Unknown action id '{}'", actionId));
        }
      }

      for (auto const& binding : componentSchema.defaultActions)
      {
        if (!binding.actionId.empty() && !schema.action(binding.actionId))
        {
          return reject(LayoutRejectionReason::UnknownAction,
                        node,
                        binding.actionId,
                        std::format("Component '{}' defaults to unknown action id '{}'", node.type, binding.actionId));
        }
      }

      return std::nullopt;
    }

    std::optional<LayoutRejection> validateNode(LayoutNode const& node,
                                                LayoutSchema const& schema,
                                                LayoutDialect const& dialect,
                                                LayoutSurface const surface,
                                                NodeIdSet& seenIds,
                                                ComponentSchema const* const parentSchema = nullptr)
    {
      auto const optComponentSchema = schema.component(node.type);

      if (!optComponentSchema)
      {
        return reject(LayoutRejectionReason::UnknownComponentType,
                      node,
                      node.type,
                      std::format("The {} schema does not register component type '{}'", dialect.name, node.type));
      }

      if (!supportsSurface(optComponentSchema->surfaces, surface))
      {
        return reject(LayoutRejectionReason::UnsupportedSurface,
                      node,
                      std::string{surface == LayoutSurface::Tooltip ? "tooltip" : "main"},
                      std::format("Component '{}' does not support the {} surface",
                                  node.type,
                                  surface == LayoutSurface::Tooltip ? "tooltip" : "main"));
      }

      if (!dialect.authorsTooltips && node.optTooltip && node.optTooltip->nodePtr)
      {
        return reject(
          LayoutRejectionReason::UnsupportedSurface,
          node,
          std::string{"tooltip"},
          std::format("{} components own their own tooltips; the schema has no tooltip surface", dialect.name));
      }

      if (surface == LayoutSurface::Tooltip && node.optTooltip && node.optTooltip->nodePtr)
      {
        return reject(LayoutRejectionReason::UnsupportedSurface,
                      node,
                      std::string{"tooltip"},
                      "A tooltip surface cannot author another tooltip");
      }

      if (node.id.empty())
      {
        if (dialect.requiresStableId != nullptr && dialect.requiresStableId(node.type))
        {
          return reject(LayoutRejectionReason::MissingRequiredId,
                        node,
                        std::string{"id"},
                        std::format("Component '{}' must carry a stable id", node.type));
        }
      }
      else if (!seenIds.insert(node.id).second)
      {
        return reject(LayoutRejectionReason::DuplicateNodeId,
                      node,
                      node.id,
                      std::format("Node id '{}' is already used in this document", node.id));
      }

      if (auto optRejection = validateActions(node, *optComponentSchema, schema); optRejection)
      {
        return optRejection;
      }

      for (auto const& [name, value] : node.props)
      {
        if (auto optRejection = validateProperty(node, *optComponentSchema, name, value); optRejection)
        {
          return optRejection;
        }
      }

      for (auto const& [name, value] : node.layout)
      {
        if (auto optRejection = validateLayoutField(node, *optComponentSchema, parentSchema, dialect, name, value);
            optRejection)
        {
          return optRejection;
        }
      }

      if (auto optRejection = validateChildCount(node, *optComponentSchema, dialect); optRejection)
      {
        return optRejection;
      }

      for (auto const& child : node.children)
      {
        if (auto optRejection = validateNode(child, schema, dialect, surface, seenIds, &*optComponentSchema);
            optRejection)
        {
          return optRejection;
        }
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        if (auto optRejection =
              validateNode(*node.optTooltip->nodePtr, schema, dialect, LayoutSurface::Tooltip, seenIds);
            optRejection)
        {
          return optRejection;
        }
      }

      return std::nullopt;
    }

    constexpr std::string_view toString(LayoutRejectionReason const reason) noexcept
    {
      switch (reason)
      {
        case LayoutRejectionReason::UnknownComponentType: return "unknown component type";
        case LayoutRejectionReason::UnsupportedLayoutField: return "unsupported layout field";
        case LayoutRejectionReason::InvalidLayoutFieldValue: return "invalid layout field value";
        case LayoutRejectionReason::UnknownProperty: return "unknown property";
        case LayoutRejectionReason::InvalidPropertyValue: return "invalid property value";
        case LayoutRejectionReason::ChildCountBelowMinimum: return "too few children";
        case LayoutRejectionReason::ChildCountAboveMaximum: return "too many children";
        case LayoutRejectionReason::MissingRequiredId: return "missing required id";
        case LayoutRejectionReason::DuplicateNodeId: return "duplicate node id";
        case LayoutRejectionReason::UnsupportedActionSlot: return "unsupported action slot";
        case LayoutRejectionReason::UnknownAction: return "unknown action";
        case LayoutRejectionReason::UnsupportedSurface: return "unsupported surface";
      }

      return "invalid layout document";
    }
  } // namespace

  std::optional<LayoutRejection> validateLayout(PreparedLayout const& layout,
                                                LayoutSchema const& schema,
                                                LayoutDialect const& dialect)
  {
    auto seenIds = NodeIdSet{};
    return validateNode(layout.effectiveRoot(), schema, dialect, LayoutSurface::Main, seenIds);
  }

  Result<> requireValidLayout(PreparedLayout const& layout, LayoutSchema const& schema, LayoutDialect const& dialect)
  {
    if (auto const optRejection = validateLayout(layout, schema, dialect); optRejection)
    {
      return makeError(Error::Code::FormatRejected, describeLayoutRejection(dialect, *optRejection));
    }

    return {};
  }

  std::string describeLayoutRejection(LayoutDialect const& dialect, LayoutRejection const& rejection)
  {
    return std::format("{} layout document rejected at '{}' ({} '{}'): {}",
                       dialect.name,
                       rejection.nodeId,
                       toString(rejection.reason),
                       rejection.detail,
                       rejection.message);
  }
} // namespace ao::uimodel
