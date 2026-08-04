// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutValidation.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
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

    constexpr auto kAllSlots = std::to_array({LayoutActionSlot::PrimaryClick,
                                              LayoutActionSlot::PrimaryLongPress,
                                              LayoutActionSlot::SecondaryClick,
                                              LayoutActionSlot::SecondaryLongPress});

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

    std::optional<LayoutRejection> validateLayoutField(LayoutNode const& node,
                                                       LayoutComponentDescriptor const& descriptor,
                                                       LayoutDialect const& dialect,
                                                       std::string const& name,
                                                       LayoutValue const& value)
    {
      // The dialect rules first: a frontend rejects another frontend's styling
      // field even where a component descriptor would have accepted it.
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

      if (std::ranges::contains(descriptor.layoutProps, name, &LayoutPropertyDescriptor::name))
      {
        return std::nullopt;
      }

      return reject(LayoutRejectionReason::UnsupportedLayoutField,
                    node,
                    name,
                    std::format("Component '{}' does not accept the layout field", node.type));
    }

    bool matchesPropertyKind(LayoutPropertyDescriptor const& propDesc, LayoutValue const& value)
    {
      switch (propDesc.kind)
      {
        case LayoutPropertyKind::Bool: return value.getIf<bool>() != nullptr;
        case LayoutPropertyKind::Int: return value.getIf<std::int64_t>() != nullptr;
        case LayoutPropertyKind::Double:
        case LayoutPropertyKind::Size: return value.isNumber();
        case LayoutPropertyKind::String:
        case LayoutPropertyKind::Enum: return value.getIf<std::string>() != nullptr;
        case LayoutPropertyKind::StringList:
          return value.getIf<std::vector<std::string>>() != nullptr || value.getIf<std::string>() != nullptr;
      }

      return false;
    }

    bool isGlobalActionProp(std::string_view const name)
    {
      return std::ranges::any_of(
        kAllSlots, [name](LayoutActionSlot const slot) { return actionPropForSlot(slot) == name; });
    }

    std::optional<LayoutRejection> validateProperty(LayoutNode const& node,
                                                    LayoutComponentDescriptor const& descriptor,
                                                    std::string const& name,
                                                    LayoutValue const& value)
    {
      if (isGlobalActionProp(name))
      {
        // Action slots carry their own policy and catalog rules.
        return std::nullopt;
      }

      auto const it = std::ranges::find(descriptor.props, name, &LayoutPropertyDescriptor::name);

      if (it == descriptor.props.end())
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
      // values are the action catalog rather than a fixed value list.
      if (it->kind == LayoutPropertyKind::Enum && !it->enumValues.empty() &&
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
                                                      LayoutComponentDescriptor const& descriptor,
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

      if (count < descriptor.minChildren)
      {
        return reject(
          LayoutRejectionReason::ChildCountBelowMinimum,
          node,
          std::string{"children"},
          std::format(
            "Component '{}' requires at least {} children, found {}", node.type, descriptor.minChildren, count));
      }

      if (descriptor.optMaxChildren && count > *descriptor.optMaxChildren)
      {
        return reject(
          LayoutRejectionReason::ChildCountAboveMaximum,
          node,
          std::string{"children"},
          std::format(
            "Component '{}' accepts at most {} children, found {}", node.type, *descriptor.optMaxChildren, count));
      }

      return std::nullopt;
    }

    std::optional<LayoutRejection> validateActions(LayoutNode const& node,
                                                   LayoutComponentDescriptor const& descriptor,
                                                   LayoutActionCatalog const& actions)
    {
      for (auto const slot : kAllSlots)
      {
        auto const propName = actionPropForSlot(slot);
        auto const it = node.props.find(propName);

        if (it == node.props.end())
        {
          continue;
        }

        if (!descriptor.actionPolicy.isSlotAllowed(slot))
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

        if (!actions.descriptor(actionId))
        {
          return reject(
            LayoutRejectionReason::UnknownAction, node, actionId, std::format("Unknown action id '{}'", actionId));
        }
      }

      for (auto const& [slot, defaultActionId] : descriptor.actionPolicy.defaultActionIds)
      {
        if (!defaultActionId.empty() && !actions.descriptor(defaultActionId))
        {
          return reject(LayoutRejectionReason::UnknownAction,
                        node,
                        defaultActionId,
                        std::format("Component '{}' defaults to unknown action id '{}'", node.type, defaultActionId));
        }
      }

      return std::nullopt;
    }

    std::optional<LayoutRejection> validateNode(LayoutNode const& node,
                                                LayoutComponentCatalog const& components,
                                                LayoutActionCatalog const& actions,
                                                LayoutDialect const& dialect,
                                                LayoutSurface const surface,
                                                NodeIdSet& seenIds)
    {
      auto const optDescriptor = components.descriptor(node.type);

      if (!optDescriptor)
      {
        return reject(LayoutRejectionReason::UnknownComponentType,
                      node,
                      node.type,
                      std::format("The {} catalog does not register component type '{}'", dialect.name, node.type));
      }

      if (!supportsSurface(optDescriptor->surfaces, surface))
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
          std::format("{} components own their own tooltips; the catalog has no tooltip surface", dialect.name));
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

      if (auto optRejection = validateActions(node, *optDescriptor, actions); optRejection)
      {
        return optRejection;
      }

      for (auto const& [name, value] : node.props)
      {
        if (auto optRejection = validateProperty(node, *optDescriptor, name, value); optRejection)
        {
          return optRejection;
        }
      }

      for (auto const& [name, value] : node.layout)
      {
        if (auto optRejection = validateLayoutField(node, *optDescriptor, dialect, name, value); optRejection)
        {
          return optRejection;
        }
      }

      if (auto optRejection = validateChildCount(node, *optDescriptor, dialect); optRejection)
      {
        return optRejection;
      }

      for (auto const& child : node.children)
      {
        if (auto optRejection = validateNode(child, components, actions, dialect, surface, seenIds); optRejection)
        {
          return optRejection;
        }
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        if (auto optRejection =
              validateNode(*node.optTooltip->nodePtr, components, actions, dialect, LayoutSurface::Tooltip, seenIds);
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
                                                LayoutComponentCatalog const& components,
                                                LayoutActionCatalog const& actions,
                                                LayoutDialect const& dialect)
  {
    auto seenIds = NodeIdSet{};
    return validateNode(layout.effectiveRoot(), components, actions, dialect, LayoutSurface::Main, seenIds);
  }

  Result<> requireValidLayout(PreparedLayout const& layout,
                              LayoutComponentCatalog const& components,
                              LayoutActionCatalog const& actions,
                              LayoutDialect const& dialect)
  {
    if (auto const optRejection = validateLayout(layout, components, actions, dialect); optRejection)
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
