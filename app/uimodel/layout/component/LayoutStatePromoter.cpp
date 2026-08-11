// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutStatePromoter.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    template<typename Visitor>
    void visitLayoutNodeMutable(LayoutNode& node, Visitor const& visitor)
    {
      visitor(node);

      for (auto& child : node.children)
      {
        visitLayoutNodeMutable(child, visitor);
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        visitLayoutNodeMutable(*node.optTooltip->nodePtr, visitor);
      }
    }

    template<typename Visitor>
    void visitLayoutDocumentMutable(LayoutDocument& doc, Visitor const& visitor)
    {
      visitLayoutNodeMutable(doc.root, visitor);

      for (auto& [templateId, templateNode] : doc.templates)
      {
        std::ignore = templateId;
        visitLayoutNodeMutable(templateNode, visitor);
      }
    }

    constexpr std::string_view kPositionPercentStateKey = "positionPercent";
    constexpr std::string_view kSizeStateKey = "size";

    // Once a state key has been folded back into the node's authored props, the
    // stored entry keeps only what is left -- or disappears when nothing is.
    void writeResidualState(LayoutNode const& node,
                            LayoutComponentStateEntry const& entry,
                            std::string_view const promotedKey,
                            LayoutComponentStateDocument& stateDoc)
    {
      auto residual = entry;
      residual.state.erase(std::string{promotedKey});

      if (residual.state.empty())
      {
        stateDoc.components.erase(node.id);
        return;
      }

      residual.baselineHash = componentBaselineHash(node);
      stateDoc.components[node.id] = std::move(residual);
    }

    bool promoteSplitState(LayoutNode& node,
                           LayoutComponentStateEntry const& entry,
                           LayoutComponentStateDocument& stateDoc)
    {
      auto const percentIt = entry.state.find(kPositionPercentStateKey);

      if (percentIt == entry.state.end() || !percentIt->second.isNumber())
      {
        return false;
      }

      auto const percent = std::clamp(percentIt->second.asDouble(), 0.0, 1.0);
      node.props.erase("position");
      node.props["initialPositionPercent"] = LayoutValue{percent};
      writeResidualState(node, entry, kPositionPercentStateKey, stateDoc);
      return true;
    }

    bool promoteCollapsibleSplitState(LayoutNode& node,
                                      LayoutComponentStateEntry const& entry,
                                      LayoutComponentStateDocument& stateDoc)
    {
      auto const sizeIt = entry.state.find(kSizeStateKey);

      if (sizeIt == entry.state.end() || !sizeIt->second.isNumber())
      {
        return false;
      }

      auto const size = std::max<std::int64_t>(50, sizeIt->second.asInt());
      node.props["position"] = LayoutValue{size};
      node.props.erase("initialPositionPercent");
      writeResidualState(node, entry, kSizeStateKey, stateDoc);
      return true;
    }
  } // namespace

  bool promotePanelSizeDefaults(LayoutDocument& doc, LayoutComponentStateDocument& stateDoc)
  {
    bool changed = false;

    visitLayoutDocumentMutable(doc,
                               [&stateDoc, &changed](LayoutNode& node)
                               {
                                 if (auto const optEntry = resolveComponentState(stateDoc, node); optEntry)
                                 {
                                   bool promoted = false;

                                   if (node.type == kSplitComponentType)
                                   {
                                     promoted = promoteSplitState(node, *optEntry, stateDoc);
                                   }
                                   else if (node.type == kCollapsibleSplitComponentType)
                                   {
                                     promoted = promoteCollapsibleSplitState(node, *optEntry, stateDoc);
                                   }

                                   if (promoted)
                                   {
                                     changed = true;
                                   }
                                 }
                               });

    return changed;
  }
} // namespace ao::uimodel
