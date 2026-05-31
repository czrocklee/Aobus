// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionValidator.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::layout
{
  namespace
  {
    void validateActionProperty(LayoutNode const& node,
                                PropertyDescriptor const& propDesc,
                                ActionCatalog const& actions,
                                ActionBindingContextResolver const& resolveBindingContext,
                                std::vector<LayoutDiagnostic>& diagnostics)
    {
      if (!propDesc.optActionBinding)
      {
        return;
      }

      auto const propIt = node.props.find(propDesc.name);

      if (propIt == node.props.end())
      {
        return;
      }

      auto const* optActionId = propIt->second.getIf<std::string>();

      if (optActionId == nullptr)
      {
        diagnostics.push_back(LayoutDiagnostic{.componentId = node.id.empty() ? node.type : node.id,
                                               .propertyName = propDesc.name,
                                               .actionId = "(invalid type)",
                                               .message = "Action ID must be a string"});
        return;
      }

      if (auto const& actionId = *optActionId; !actionId.empty() && actionId != "none")
      {
        auto const optBindCtx = resolveBindingContext(node, propDesc);

        if (!optBindCtx)
        {
          return;
        }

        if (!actions.canBind(actionId, *optBindCtx))
        {
          diagnostics.push_back(LayoutDiagnostic{.componentId = node.id.empty() ? node.type : node.id,
                                                 .propertyName = propDesc.name,
                                                 .actionId = actionId,
                                                 .message = "Unknown or incompatible action ID: " + actionId});
        }
      }
    }

    void validateNode(LayoutNode const& node,
                      ComponentCatalog const& components,
                      ActionCatalog const& actions,
                      ActionBindingContextResolver const& resolveBindingContext,
                      std::vector<LayoutDiagnostic>& diagnostics)
    {
      if (auto const optCompDesc = components.descriptor(node.type); optCompDesc)
      {
        for (auto const& propDesc : optCompDesc->props)
        {
          validateActionProperty(node, propDesc, actions, resolveBindingContext, diagnostics);
        }

        // Phase 1: Diagnose disallowed global action properties
        static constexpr auto kGlobalActionProps = std::to_array<std::string_view>(
          {kPrimaryActionProp, kSecondaryActionProp, kPrimaryLongPressActionProp, kSecondaryLongPressActionProp});

        for (auto const propName : kGlobalActionProps)
        {
          if (node.props.contains(propName))
          {
            bool const supported =
              std::any_of(optCompDesc->props.begin(),
                          optCompDesc->props.end(),
                          [propName](PropertyDescriptor const& prop) { return prop.name == propName; });

            if (!supported)
            {
              diagnostics.push_back(
                LayoutDiagnostic{.componentId = node.id.empty() ? node.type : node.id,
                                 .propertyName = std::string{propName},
                                 .actionId = "",
                                 .message = "Action slot is not supported by this component policy"});
            }
          }
        }
      }

      for (auto const& child : node.children)
      {
        validateNode(child, components, actions, resolveBindingContext, diagnostics);
      }
    }
  } // namespace

  std::vector<LayoutDiagnostic> validateActions(LayoutDocument const& doc,
                                                ComponentCatalog const& components,
                                                ActionCatalog const& actions,
                                                ActionBindingContextResolver const& resolveBindingContext)
  {
    auto diagnostics = std::vector<LayoutDiagnostic>{};
    validateNode(doc.root, components, actions, resolveBindingContext, diagnostics);
    return diagnostics;
  }
} // namespace ao::uimodel::layout
