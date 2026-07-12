// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/track/TrackSelectionRegionPolicy.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackSelectionRegionComponent final : public LayoutComponent
    {
    public:
      TrackSelectionRegionComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}
        , _showWhen{node.propertyOr<std::string>("showWhen", "any")}
        , _showPlaceholder{node.propertyOr<bool>("showPlaceholder", false)}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        if (ctx.detailScope != nullptr)
        {
          _scopeConn =
            ctx.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { updateVisibility(snap); });
          updateVisibility(ctx.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateVisibility(rt::TrackDetailSnapshot const& snap)
      {
        auto const presentation = presentationForTrackSelectionRegion(snap.selectionKind, _showWhen, _showPlaceholder);
        _box.set_visible(presentation.visible);
        _box.set_sensitive(presentation.sensitive);
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<LayoutComponent>> _children;
      std::string _showWhen;
      bool _showPlaceholder = false;
      sigc::connection _scopeConn;
    };

    std::unique_ptr<LayoutComponent> createTrackSelectionRegion(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackSelectionRegionComponent>(ctx, node);
    }
  } // namespace

  void registerTrackSelectionRegionComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.selectionRegion",
                                .displayName = "Selection Region",
                                .category = LayoutComponentCategory::Track,
                                .props = {{.name = "showWhen",
                                           .kind = LayoutPropertyKind::String,
                                           .label = "Show When",
                                           .defaultValue = LayoutValue{"any"}},
                                          {.name = "showPlaceholder",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Show Placeholder",
                                           .defaultValue = LayoutValue{false}}},
                                .minChildren = 1,
                                .optMaxChildren = 0},
                               createTrackSelectionRegion);
  }
} // namespace ao::gtk::layout
