// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackSelectionRegionPolicy.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    class TrackSelectionRegionComponent final : public ILayoutComponent
    {
    public:
      TrackSelectionRegionComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL, 0}
        , _showWhen{node.getProp<std::string>("showWhen", "any")}
        , _showPlaceholder{node.getProp<bool>("showPlaceholder", false)}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }

        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn = ctx.track.detailScope->signalSnapshotChanged().connect([this](auto const& snap)
                                                                              { updateVisibility(snap); });
          updateVisibility(ctx.track.detailScope->snapshot());
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      void updateVisibility(rt::TrackDetailSnapshot const& snap)
      {
        auto const presentation =
          track_selection_region::presentationForSelection(snap.selectionKind, _showWhen, _showPlaceholder);
        _box.set_visible(presentation.visible);
        _box.set_sensitive(presentation.sensitive);
      }

      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
      std::string _showWhen;
      bool _showPlaceholder = false;
      sigc::connection _scopeConn;
    };

    std::unique_ptr<ILayoutComponent> createTrackSelectionRegion(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackSelectionRegionComponent>(ctx, node);
    }
  } // namespace

  void registerTrackSelectionRegionComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.selectionRegion",
       .displayName = "Selection Region",
       .category = ComponentCategory::Track,
       .props =
         {{.name = "showWhen", .kind = PropertyKind::String, .label = "Show When", .defaultValue = LayoutValue{"any"}},
          {.name = "showPlaceholder",
           .kind = PropertyKind::Bool,
           .label = "Show Placeholder",
           .defaultValue = LayoutValue{false}}},
       .layoutProps = {},
       .minChildren = 1,
       .optMaxChildren = 0},
      createTrackSelectionRegion);
  }
} // namespace ao::gtk::layout
