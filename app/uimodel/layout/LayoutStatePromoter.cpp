// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutStatePromoter.h>
#include <ao/uimodel/layout/StatefulLayoutComponentType.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::layout
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

    bool promoteSplitState(LayoutNode& node,
                           LayoutComponentStateEntry const& entry,
                           LayoutComponentStateDocument& stateDoc)
    {
      auto const percentIt = entry.state.find("positionPercent");

      if (percentIt == entry.state.end() || !percentIt->second.isNumber())
      {
        return false;
      }

      auto const percent = std::clamp(percentIt->second.asDouble(), 0.0, 1.0);
      node.props.erase("position");
      node.props["initialPositionPercent"] = LayoutValue{percent};

      auto residual = entry;
      residual.state.erase("positionPercent");

      if (residual.state.empty())
      {
        stateDoc.components.erase(node.id);
      }
      else
      {
        residual.baselineHash = layoutComponentBaselineHash(node);
        stateDoc.components[node.id] = std::move(residual);
      }

      return true;
    }

    bool promoteCollapsibleSplitState(LayoutNode& node,
                                      LayoutComponentStateEntry const& entry,
                                      LayoutComponentStateDocument& stateDoc)
    {
      auto const sizeIt = entry.state.find("size");

      if (sizeIt == entry.state.end() || !sizeIt->second.isNumber())
      {
        return false;
      }

      auto const size = std::max<std::int64_t>(50, sizeIt->second.asInt());
      node.props["position"] = LayoutValue{size};
      node.props.erase("initialPositionPercent");

      auto residual = entry;
      residual.state.erase("size");

      if (residual.state.empty())
      {
        stateDoc.components.erase(node.id);
      }
      else
      {
        residual.baselineHash = layoutComponentBaselineHash(node);
        stateDoc.components[node.id] = std::move(residual);
      }

      return true;
    }
  } // namespace

  PanelSizePromotionResult promotePanelSizeDefaults(LayoutDocument& doc, LayoutComponentStateDocument& stateDoc)
  {
    auto result = PanelSizePromotionResult{};

    visitLayoutDocumentMutable(doc,
                               [&stateDoc, &result](LayoutNode& node)
                               {
                                 if (auto const optEntry = resolveLayoutComponentState(stateDoc, node); optEntry)
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
                                     result.changed = true;
                                     ++result.promotedCount;
                                   }
                                 }
                               });

    result.residualCount = stateDoc.components.size();
    return result;
  }
} // namespace ao::uimodel::layout
