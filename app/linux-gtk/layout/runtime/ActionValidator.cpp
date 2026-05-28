// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include "ActionValidator.h"

#include "ComponentRegistry.h"
#include "ActionRegistry.h"
#include "layout/document/LayoutNode.h"
#include "layout/document/LayoutDocument.h"

#include <string>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    void validateActionProperty(LayoutNode const& node,
                                PropertyDescriptor const& propDesc,
                                ActionRegistry const& actionRegistry,
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

      const auto* const optActionId = propIt->second.getIf<std::string>();

      if (optActionId == nullptr)
      {
        diagnostics.push_back(LayoutDiagnostic{
          .componentId = node.id.empty() ? node.type : node.id,
          .propertyName = propDesc.name,
          .actionId = "(invalid type)",
          .message = "Action ID must be a string"
        });
        return;
      }

      if (auto const& actionId = *optActionId; !actionId.empty() && actionId != "none")
      {
        // TODO: hasAnchor and hasFocusedView are hardcoded for Phase 1 widget bindings
        auto const bindCtx = ActionBindingContext{
          .slot = propDesc.optActionBinding->slot,
          .hasAnchor = true,
          .hasFocusedView = true,
          .componentType = node.type
        };

        if (!actionRegistry.canBind(actionId, bindCtx))
        {
          diagnostics.push_back(LayoutDiagnostic{
            .componentId = node.id.empty() ? node.type : node.id,
            .propertyName = propDesc.name,
            .actionId = actionId,
            .message = "Unknown or incompatible action ID: " + actionId
          });
        }
      }
    }

    void validateNode(LayoutNode const& node,
                      ComponentRegistry const& compRegistry,
                      ActionRegistry const& actionRegistry,
                      std::vector<LayoutDiagnostic>& diagnostics)
    {
      if (auto const optCompDesc = compRegistry.descriptor(node.type))
      {
        for (auto const& propDesc : optCompDesc->props)
        {
          validateActionProperty(node, propDesc, actionRegistry, diagnostics);
        }
      }

      for (auto const& child : node.children)
      {
        validateNode(child, compRegistry, actionRegistry, diagnostics);
      }
    }
  } // namespace

  std::vector<LayoutDiagnostic> validateActions(LayoutDocument const& doc,
                                                ComponentRegistry const& compRegistry,
                                                ActionRegistry const& actionRegistry)
  {
    auto diagnostics = std::vector<LayoutDiagnostic>{};
    validateNode(doc.root, compRegistry, actionRegistry, diagnostics);
    return diagnostics;
  }
} // namespace ao::gtk::layout
